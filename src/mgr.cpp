#include "mgr.hpp"
#include "sim.hpp"

#include <madrona/utils.hpp>
#include <madrona/importer.hpp>
#include <madrona/physics_loader.hpp>
#include <madrona/tracing.hpp>
#include <madrona/mw_cpu.hpp>

#include <array>
#include <charconv>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef MADRONA_CUDA_SUPPORT
#include <madrona/mw_gpu.hpp>
#include <madrona/cuda_utils.hpp>
#endif

using namespace madrona;
using namespace madrona::math;
using namespace madrona::phys;
using namespace madrona::py;

namespace madEscape {

struct Manager::Impl {
    Config cfg;
    PhysicsLoader physicsLoader;
    EpisodeManager *episodeMgr;
    WorldReset *worldResetBuffer;
    CheckpointSave *worldSaveCheckpointBuffer;
    CheckpointReset *worldLoadCheckpointBuffer;
    Action *agentActionsBuffer;

    inline Impl(const Manager::Config &mgr_cfg,
                PhysicsLoader &&phys_loader,
                EpisodeManager *ep_mgr,
                WorldReset *reset_buffer,
                CheckpointSave *checkpoint_save_buffer,
                CheckpointReset *checkpoint_load_buffer,
                Action *action_buffer)
        : cfg(mgr_cfg),
          physicsLoader(std::move(phys_loader)),
          episodeMgr(ep_mgr),
          worldResetBuffer(reset_buffer),
          worldSaveCheckpointBuffer(checkpoint_save_buffer),
          worldLoadCheckpointBuffer(checkpoint_load_buffer),
          agentActionsBuffer(action_buffer)
    {}

    inline virtual ~Impl() {}

    virtual void run() = 0;

#ifdef MADRONA_CUDA_SUPPORT
    virtual void gpuRollout(cudaStream_t strm, void **buffers,
                            const TrainInterface &train_iface) = 0;
#endif

    virtual Tensor exportTensor(ExportID slot,
        Tensor::ElementType type,
        madrona::Span<const int64_t> dimensions) const = 0;

    static inline Impl * init(const Config &cfg,
                              const viz::VizECSBridge *viz_bridge);
};

struct Manager::CPUImpl final : Manager::Impl {
    using TaskGraphT =
        TaskGraphExecutor<Engine, Sim, Sim::Config, WorldInit>;

    TaskGraphT cpuExec;

    inline CPUImpl(const Manager::Config &mgr_cfg,
                   PhysicsLoader &&phys_loader,
                   EpisodeManager *ep_mgr,
                   WorldReset *reset_buffer,
                   CheckpointSave *checkpoint_save_buffer,
                   CheckpointReset *checkpoint_load_buffer,
                   Action *action_buffer,
                   TaskGraphT &&cpu_exec)
        : Impl(mgr_cfg, std::move(phys_loader),
               ep_mgr, reset_buffer, checkpoint_save_buffer,
               checkpoint_load_buffer, action_buffer),
          cpuExec(std::move(cpu_exec))
    {}

    inline virtual ~CPUImpl() final
    {
        delete episodeMgr;
    }

    inline virtual void run()
    {
        cpuExec.run();
    }

#ifdef MADRONA_CUDA_SUPPORT
    virtual void gpuRollout(cudaStream_t strm, void **buffers,
                            const TrainInterface &train_iface)
    {
        (void)strm;
        (void)buffers;
        (void)train_iface;
        assert(false);
    }
#endif

    virtual inline Tensor exportTensor(ExportID slot,
        Tensor::ElementType type,
        madrona::Span<const int64_t> dims) const final
    {
        void *dev_ptr = cpuExec.getExported((uint32_t)slot);
        return Tensor(dev_ptr, type, dims, Optional<int>::none());
    }
};

#ifdef MADRONA_CUDA_SUPPORT
struct Manager::CUDAImpl final : Manager::Impl {
    MWCudaExecutor gpuExec;

    inline CUDAImpl(const Manager::Config &mgr_cfg,
                   PhysicsLoader &&phys_loader,
                   EpisodeManager *ep_mgr,
                   WorldReset *reset_buffer,
                   CheckpointSave *checkpoint_save_buffer,
                   CheckpointReset *checkpoint_load_buffer,
                   Action *action_buffer,
                   MWCudaExecutor &&gpu_exec)
        : Impl(mgr_cfg, std::move(phys_loader),
               ep_mgr, reset_buffer, checkpoint_save_buffer,
               checkpoint_load_buffer, action_buffer),
          gpuExec(std::move(gpu_exec))
    {}

    inline virtual ~CUDAImpl() final
    {
        REQ_CUDA(cudaFree(episodeMgr));
    }

    inline virtual void run()
    {
        gpuExec.run();
    }

#ifdef MADRONA_CUDA_SUPPORT
    virtual void gpuRollout(cudaStream_t strm, void **buffers,
                            const TrainInterface &train_iface)
    {
        auto numTensorBytes = [](const Tensor &t) {
            uint64_t num_items = 1;
            uint64_t num_dims = t.numDims();
            for (uint64_t i = 0; i < num_dims; i++) {
                num_items *= t.dims()[i];
            }

            return num_items * (uint64_t)t.numBytesPerItem();
        };

        auto copyToSim = [&strm, &numTensorBytes](const Tensor &dst, void *src) {
            uint64_t num_bytes = numTensorBytes(dst);

            REQ_CUDA(cudaMemcpyAsync(dst.devicePtr(), src, num_bytes,
                cudaMemcpyDeviceToDevice, strm));
        };

        auto copyFromSim = [&strm, &numTensorBytes](void *dst, const Tensor &src) {
            uint64_t num_bytes = numTensorBytes(src);

            REQ_CUDA(cudaMemcpyAsync(dst, src.devicePtr(), num_bytes,
                cudaMemcpyDeviceToDevice, strm));
        };

        Span<const TrainInterface::NamedTensor> src_obs =
            train_iface.observations();
        Span<const TrainInterface::NamedTensor> src_stats =
            train_iface.stats();
        auto policy_assignments = train_iface.policyAssignments();

        void **input_buffers = buffers;
        void **output_buffers = buffers +
            src_obs.size() + src_stats.size() + 4;

        if (policy_assignments.has_value()) {
            output_buffers += 1;
        }

        CountT cur_idx = 0;

        copyToSim(train_iface.actions(), input_buffers[cur_idx++]);
        copyToSim(train_iface.resets(), input_buffers[cur_idx++]);

        gpuExec.runAsync(strm);

        copyFromSim(output_buffers[cur_idx++], train_iface.rewards());
        copyFromSim(output_buffers[cur_idx++], train_iface.dones());

        if (policy_assignments.has_value()) {
            copyFromSim(output_buffers[cur_idx++], *policy_assignments);
        }

        for (const TrainInterface::NamedTensor &t : src_obs) {
            copyFromSim(output_buffers[cur_idx++], t.hdl);
        }

        for (const TrainInterface::NamedTensor &t : src_stats) {
            copyFromSim(output_buffers[cur_idx++], t.hdl);
        }
    }
#endif

    virtual inline Tensor exportTensor(ExportID slot,
        Tensor::ElementType type,
        madrona::Span<const int64_t> dims) const final
    {
        void *dev_ptr = gpuExec.getExported((uint32_t)slot);
        return Tensor(dev_ptr, type, dims, cfg.gpuID);
    }
};
#endif

static void loadPhysicsObjects(PhysicsLoader &loader)
{
    std::array<std::string, (size_t)SimObject::NumObjects - 1> asset_paths;
    asset_paths[(size_t)SimObject::Cube] =
        (std::filesystem::path(DATA_DIR) / "cube_collision.obj").string();
    asset_paths[(size_t)SimObject::Wall] =
        (std::filesystem::path(DATA_DIR) / "wall_collision.obj").string();
    asset_paths[(size_t)SimObject::Door] =
        (std::filesystem::path(DATA_DIR) / "wall_collision.obj").string();
    asset_paths[(size_t)SimObject::Agent] =
        (std::filesystem::path(DATA_DIR) / "agent_collision_simplified.obj").string();
    asset_paths[(size_t)SimObject::Button] =
        (std::filesystem::path(DATA_DIR) / "cube_collision.obj").string();
    asset_paths[(size_t)SimObject::Key] =
        (std::filesystem::path(DATA_DIR) / "cube_collision.obj").string();


    std::array<const char *, (size_t)SimObject::NumObjects - 1> asset_cstrs;
    for (size_t i = 0; i < asset_paths.size(); i++) {
        asset_cstrs[i] = asset_paths[i].c_str();
    }

    char import_err_buffer[4096];
    auto imported_src_hulls = imp::ImportedAssets::importFromDisk(
        asset_cstrs, import_err_buffer, true);

    if (!imported_src_hulls.has_value()) {
        FATAL("%s", import_err_buffer);
    }

    DynArray<imp::SourceMesh> src_convex_hulls(
        imported_src_hulls->objects.size());

    DynArray<DynArray<SourceCollisionPrimitive>> prim_arrays(0);
    HeapArray<SourceCollisionObject> src_objs(
        (CountT)SimObject::NumObjects);

    auto setupHull = [&](SimObject obj_id,
                         float inv_mass,
                         RigidBodyFrictionData friction) {
        auto meshes = imported_src_hulls->objects[(CountT)obj_id].meshes;
        DynArray<SourceCollisionPrimitive> prims(meshes.size());

        for (const imp::SourceMesh &mesh : meshes) {
            src_convex_hulls.push_back(mesh);
            prims.push_back({
                .type = CollisionPrimitive::Type::Hull,
                .hullInput = {
                    .hullIDX = uint32_t(src_convex_hulls.size() - 1),
                },
            });
        }

        prim_arrays.emplace_back(std::move(prims));

        src_objs[(CountT)obj_id] = SourceCollisionObject {
            .prims = Span<const SourceCollisionPrimitive>(prim_arrays.back()),
            .invMass = inv_mass,
            .friction = friction,
        };
    };

    setupHull(SimObject::Cube, 0.075f, {
        .muS = 0.5f,
        .muD = 0.75f,
    });

    setupHull(SimObject::Wall, 0.f, {
        .muS = 0.5f,
        .muD = 0.5f,
    });

    setupHull(SimObject::Door, 0.f, {
        .muS = 0.5f,
        .muD = 0.5f,
    });

    setupHull(SimObject::Agent, 1.f, {
        .muS = 0.5f,
        .muD = 0.5f,
    });

    setupHull(SimObject::Button, 1.f, {
        .muS = 0.5f,
        .muD = 0.5f,
    });

    setupHull(SimObject::Key, 1.f, {
        .muS = 0.5f,
        .muD = 0.5f,
    });

    SourceCollisionPrimitive plane_prim {
        .type = CollisionPrimitive::Type::Plane,
    };

    src_objs[(CountT)SimObject::Plane] = {
        .prims = Span<const SourceCollisionPrimitive>(&plane_prim, 1),
        .invMass = 0.f,
        .friction = {
            .muS = 0.5f,
            .muD = 0.5f,
        },
    };



    StackAlloc tmp_alloc;
    RigidBodyAssets rigid_body_assets;
    CountT num_rigid_body_data_bytes;
    void *rigid_body_data = RigidBodyAssets::processRigidBodyAssets(
        src_convex_hulls,
        src_objs,
        false,
        tmp_alloc,
        &rigid_body_assets,
        &num_rigid_body_data_bytes);

    if (rigid_body_data == nullptr) {
        FATAL("Invalid collision hull input");
    }

    // This is a bit hacky, but in order to make sure the agents
    // remain controllable by the policy, they are only allowed to
    // rotate around the Z axis (infinite inertia in x & y axes)
    rigid_body_assets.metadatas[
        (CountT)SimObject::Agent].mass.invInertiaTensor.x = 0.f;
    rigid_body_assets.metadatas[
        (CountT)SimObject::Agent].mass.invInertiaTensor.y = 0.f;

    loader.loadRigidBodies(rigid_body_assets);
    free(rigid_body_data);
}

Manager::Impl * Manager::Impl::init(
    const Manager::Config &mgr_cfg,
    const viz::VizECSBridge *viz_bridge)
{
    Sim::Config sim_cfg {
        viz_bridge != nullptr,
        mgr_cfg.autoReset,
        mgr_cfg.simFlags,
        mgr_cfg.rewardMode,
        mgr_cfg.buttonWidth,
        mgr_cfg.doorWidth,
    };

    switch (mgr_cfg.execMode) {
    case ExecMode::CUDA: {
#ifdef MADRONA_CUDA_SUPPORT
        CUcontext cu_ctx = MWCudaExecutor::initCUDA(mgr_cfg.gpuID);

        EpisodeManager *episode_mgr = 
            (EpisodeManager *)cu::allocGPU(sizeof(EpisodeManager));
        REQ_CUDA(cudaMemset(episode_mgr, 0, sizeof(EpisodeManager)));

        // Allocate what I want here with allocGPU
        float *progress_ptr = (float *)cu::allocGPU(sizeof(float));
        REQ_CUDA(cudaMemset(progress_ptr, 0, sizeof(float)));

        PhysicsLoader phys_loader(ExecMode::CUDA, 10);
        loadPhysicsObjects(phys_loader);

        ObjectManager *phys_obj_mgr = &phys_loader.getObjectManager();

        HeapArray<WorldInit> world_inits(mgr_cfg.numWorlds);

        for (int64_t i = 0; i < (int64_t)mgr_cfg.numWorlds; i++) {
            world_inits[i] = WorldInit {
                episode_mgr,
                phys_obj_mgr,
                viz_bridge,
                progress_ptr, // Add progress_ptr to WorldInit
            };
        }

        MWCudaExecutor gpu_exec({
            .worldInitPtr = world_inits.data(),
            .numWorldInitBytes = sizeof(WorldInit),
            .userConfigPtr = (void *)&sim_cfg,
            .numUserConfigBytes = sizeof(Sim::Config),
            .numWorldDataBytes = sizeof(Sim),
            .worldDataAlignment = alignof(Sim),
            .numWorlds = mgr_cfg.numWorlds,
            .numExportedBuffers = (uint32_t)ExportID::NumExports, 
        }, {
            { GPU_HIDESEEK_SRC_LIST },
            { GPU_HIDESEEK_COMPILE_FLAGS },
            CompileConfig::OptMode::LTO,
        }, cu_ctx);

        WorldReset *world_reset_buffer = 
            (WorldReset *)gpu_exec.getExported((uint32_t)ExportID::Reset);

        CheckpointSave *checkpoint_save_buffer = 
            (CheckpointSave *)gpu_exec.getExported((uint32_t)ExportID::CheckpointSave);

        CheckpointReset *checkpoint_load_buffer = 
            (CheckpointReset *)gpu_exec.getExported((uint32_t)ExportID::CheckpointReset);

        Action *agent_actions_buffer = 
            (Action *)gpu_exec.getExported((uint32_t)ExportID::Action);


        return new CUDAImpl {
            mgr_cfg,
            std::move(phys_loader),
            episode_mgr,
            world_reset_buffer,
            checkpoint_save_buffer,
            checkpoint_load_buffer,
            agent_actions_buffer,
            std::move(gpu_exec),
        };
#else
        FATAL("Madrona was not compiled with CUDA support");
#endif
    } break;
    case ExecMode::CPU: {
        EpisodeManager *episode_mgr = new EpisodeManager { 0 };

        // Allocate what I want here on heap
        float *progress_ptr = new float(0.f);

        PhysicsLoader phys_loader(ExecMode::CPU, 10);
        loadPhysicsObjects(phys_loader);

        ObjectManager *phys_obj_mgr = &phys_loader.getObjectManager();

        HeapArray<WorldInit> world_inits(mgr_cfg.numWorlds);

        for (int64_t i = 0; i < (int64_t)mgr_cfg.numWorlds; i++) {
            world_inits[i] = WorldInit {
                episode_mgr,
                phys_obj_mgr,
                viz_bridge,
                progress_ptr, // CPU version
            };
        }

        CPUImpl::TaskGraphT cpu_exec {
            ThreadPoolExecutor::Config {
                .numWorlds = mgr_cfg.numWorlds,
                .numExportedBuffers = (uint32_t)ExportID::NumExports,
            },
            sim_cfg,
            world_inits.data(),
        };

        WorldReset *world_reset_buffer = 
            (WorldReset *)cpu_exec.getExported((uint32_t)ExportID::Reset);

        CheckpointSave *checkpoint_save_buffer = 
            (CheckpointSave *)cpu_exec.getExported((uint32_t)ExportID::CheckpointSave);

        CheckpointReset *checkpoint_load_buffer = 
            (CheckpointReset *)cpu_exec.getExported((uint32_t)ExportID::CheckpointReset);

        Action *agent_actions_buffer = 
            (Action *)cpu_exec.getExported((uint32_t)ExportID::Action);

        auto cpu_impl = new CPUImpl {
            mgr_cfg,
            std::move(phys_loader),
            episode_mgr,
            world_reset_buffer,
            checkpoint_save_buffer,
            checkpoint_load_buffer,
            agent_actions_buffer,
            std::move(cpu_exec),
        };

        return cpu_impl;
    } break;
    default: MADRONA_UNREACHABLE();
    }
}

Manager::Manager(const Config &cfg,
                 const viz::VizECSBridge *viz_bridge)
    : impl_(Impl::init(cfg, viz_bridge))
{
    // Currently, there is no way to populate the initial set of observations
    // without stepping the simulations in order to execute the taskgraph.
    // Therefore, after setup, we step all the simulations with a forced reset
    // that ensures the first real step will have valid observations at the
    // start of a fresh episode in order to compute actions.
    //
    // This will be improved in the future with support for multiple task
    // graphs, allowing a small task graph to be executed after initialization.
    
    for (int32_t i = 0; i < (int32_t)cfg.numWorlds; i++) {
        triggerReset(i);
    }

    step();
}

Manager::~Manager() {}

void Manager::step()
{
    impl_->run();
}

#ifdef MADRONA_CUDA_SUPPORT
void Manager::gpuRolloutStep(cudaStream_t strm, void **rollout_buffers)
{
    TrainInterface iface = trainInterface();
    impl_->gpuRollout(strm, rollout_buffers, iface);
}
#endif
Tensor Manager::checkpointResetTensor() const {
    return impl_->exportTensor(ExportID::CheckpointReset,
                               Tensor::ElementType::Int32,
                               {
                                   impl_->cfg.numWorlds,
                                   1
                               });
}

Tensor Manager::checkpointTensor() const {
    return impl_->exportTensor(ExportID::Checkpoint,
                               Tensor::ElementType::UInt8,
                               {
                                   impl_->cfg.numWorlds,
                                   sizeof(Checkpoint)
                               });
}

Tensor Manager::resetTensor() const
{
    return impl_->exportTensor(ExportID::Reset,
                               Tensor::ElementType::Int32,
                               {
                                   impl_->cfg.numWorlds,
                                   1,
                               });
}

Tensor Manager::actionTensor() const
{
    return impl_->exportTensor(ExportID::Action, Tensor::ElementType::Int32,
        {
            impl_->cfg.numWorlds * consts::numAgents,
            4,
        });
}

Tensor Manager::rewardTensor() const
{
    return impl_->exportTensor(ExportID::Reward, Tensor::ElementType::Float32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   1,
                               });
}

Tensor Manager::doneTensor() const
{
    return impl_->exportTensor(ExportID::Done, Tensor::ElementType::Int32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   1,
                               });
}

Tensor Manager::selfObservationTensor() const
{
    return impl_->exportTensor(ExportID::SelfObservation,
                               Tensor::ElementType::Float32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   sizeof(SelfObservation) / sizeof(float),
                               });
}

Tensor Manager::partnerObservationsTensor() const
{
    return impl_->exportTensor(ExportID::PartnerObservations,
                               Tensor::ElementType::Float32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   consts::numAgents - 1,
                                   3,
                               });
}

Tensor Manager::roomEntityObservationsTensor() const
{
    return impl_->exportTensor(ExportID::RoomEntityObservations,
                               Tensor::ElementType::Float32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   consts::maxObservationsPerAgent,
                                   3,
                               });
}

Tensor Manager::doorObservationTensor() const
{
    return impl_->exportTensor(ExportID::DoorObservation,
                               Tensor::ElementType::Float32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   1,
                                   3,
                               });
}

Tensor Manager::lidarTensor() const
{
    return impl_->exportTensor(ExportID::Lidar, Tensor::ElementType::Float32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   consts::numLidarSamples,
                                   2,
                               });
}

Tensor Manager::stepsRemainingTensor() const
{
    return impl_->exportTensor(ExportID::StepsRemaining,
                               Tensor::ElementType::Int32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   1,
                               });
}

Tensor Manager::agentIDTensor() const
{
    return impl_->exportTensor(ExportID::AgentID,
                               Tensor::ElementType::Int32,
                               {
                                   impl_->cfg.numWorlds * consts::numAgents,
                                   1,
                               });
}

TrainInterface Manager::trainInterface() const
{
    return TrainInterface {
        {
            { "self", selfObservationTensor() },
            { "partners", partnerObservationsTensor() },
            { "roomEntities", roomEntityObservationsTensor() },
            { "door", doorObservationTensor() },
            { "lidar", lidarTensor() },
            { "stepsRemaining", stepsRemainingTensor() },
            { "agentID", agentIDTensor() },
        },
        actionTensor(),
        rewardTensor(),
        doneTensor(),
        resetTensor(),
        Optional<Tensor>::none(),
    };
}

void Manager::triggerReset(int32_t world_idx)
{
    WorldReset reset {
        1,
    };

    auto *reset_ptr = impl_->worldResetBuffer + world_idx;

    if (impl_->cfg.execMode == ExecMode::CUDA) {
#ifdef MADRONA_CUDA_SUPPORT
        cudaMemcpy(reset_ptr, &reset, sizeof(WorldReset),
                   cudaMemcpyHostToDevice);
#endif
    }  else {
        *reset_ptr = reset;
    }
}

void Manager::setAction(int32_t world_idx,
                        int32_t agent_idx,
                        int32_t move_amount,
                        int32_t move_angle,
                        int32_t rotate,
                        int32_t interact)
{
    Action action { 
        .moveAmount = move_amount,
        .moveAngle = move_angle,
        .rotate = rotate,
        .interact = interact
    };

    auto *action_ptr = impl_->agentActionsBuffer +
        world_idx * consts::numAgents + agent_idx;

    if (impl_->cfg.execMode == ExecMode::CUDA) {
#ifdef MADRONA_CUDA_SUPPORT
        cudaMemcpy(action_ptr, &action, sizeof(Action),
                   cudaMemcpyHostToDevice);
#endif
    } else {
        *action_ptr = action;
    }
}

void Manager::setSaveCheckpoint(int32_t world_idx, int32_t value) 
{
    CheckpointSave save {
        value,
    };

    auto *save_ptr = impl_->worldSaveCheckpointBuffer + world_idx;

    if (impl_->cfg.execMode == ExecMode::CUDA) {
#ifdef MADRONA_CUDA_SUPPORT
        cudaMemcpy(save_ptr, &save, sizeof(CheckpointSave),
                   cudaMemcpyHostToDevice);
#endif
    }  else {
        *save_ptr = save;
    }
}

void Manager::triggerLoadCheckpoint(int32_t world_idx) 
{
    CheckpointReset load {
        1,
    };

    auto *load_ptr = impl_->worldLoadCheckpointBuffer + world_idx;

    if (impl_->cfg.execMode == ExecMode::CUDA) {
#ifdef MADRONA_CUDA_SUPPORT
        cudaMemcpy(load_ptr, &load, sizeof(CheckpointReset),
                   cudaMemcpyHostToDevice);
#endif
    }  else {
        *load_ptr = load;
    }
}

}
