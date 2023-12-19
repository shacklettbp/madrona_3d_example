/*#include <memory>

#include <madrona/py/utils.hpp>
#include <madrona/exec_mode.hpp>

#include <madrona/render/render_mgr.hpp>

namespace madEscape {

// The Manager class encapsulates the linkage between the outside training
// code and the internal simulation state (src/sim.hpp / src/sim.cpp)
//
// Manager is responsible for initializing the simulator, loading physics
// and rendering assets off disk, and mapping ECS components to tensors
// for learning
class Manager {
public:
    struct Config {
        madrona::ExecMode execMode; // CPU or CUDA
        int gpuID; // Which GPU for CUDA backend?
        uint32_t numWorlds; // Simulation batch size
        bool autoReset; // Immediately generate new world on episode end
        bool enableBatchRenderer;
        uint32_t batchRenderViewWidth = 64;
        uint32_t batchRenderViewHeight = 64;
        madrona::render::APIBackend *extRenderAPI = nullptr;
        madrona::render::GPUDevice *extRenderDev = nullptr;
    };

    Manager(const Config &cfg);
    ~Manager();

    void step();

    // These functions export Tensor objects that link the ECS
    // simulation state to the python bindings / PyTorch tensors (src/bindings.cpp)
    madrona::py::Tensor resetTensor() const;
    madrona::py::Tensor actionTensor() const;
    madrona::py::Tensor rewardTensor() const;
    madrona::py::Tensor doneTensor() const;
    madrona::py::Tensor selfObservationTensor() const;
    madrona::py::Tensor partnerObservationsTensor() const;
    madrona::py::Tensor roomEntityObservationsTensor() const;
    madrona::py::Tensor doorObservationTensor() const;
    madrona::py::Tensor lidarTensor() const;
    madrona::py::Tensor stepsRemainingTensor() const;
    madrona::py::Tensor rgbTensor() const;
    madrona::py::Tensor depthTensor() const;

    // These functions are used by the viewer to control the simulation
    // with keyboard inputs in place of DNN policy actions
    void triggerReset(int32_t world_idx);
    void setAction(int32_t world_idx,
                   int32_t agent_idx,
                   int32_t move_amount,
                   int32_t move_angle,
                   int32_t rotate,
                   int32_t grab);

    madrona::render::RenderManager & getRenderManager();

private:
    struct Impl;
    struct CPUImpl;
    struct CUDAImpl;

    std::unique_ptr<Impl> impl_;
};

}
*/

#pragma once
#ifdef madrona_3d_example_mgr_EXPORTS
#define MGR_EXPORT MADRONA_EXPORT
#else
#define MGR_EXPORT MADRONA_IMPORT
#endif

#include <memory>

#include <madrona/py/utils.hpp>
#include <madrona/exec_mode.hpp>

#include <madrona/render/mw.hpp>
#include <madrona/viz/system.hpp>

namespace madEscape {

// The Manager class encapsulates the linkage between the outside training
// code and the internal simulation state (src/sim.hpp / src/sim.cpp)
//
// Manager is responsible for initializing the simulator, loading physics
// and rendering assets off disk, and mapping ECS components to tensors
// for learning
class Manager {
public:
    struct Config {
        madrona::ExecMode execMode; // CPU or CUDA
        int gpuID; // Which GPU for CUDA backend?
        uint32_t numWorlds; // Simulation batch size
        bool autoReset; // Immediately generate new world on episode end
    };

    MGR_EXPORT Manager(
        const Config &cfg,
        const madrona::viz::VizECSBridge *viz_bridge = nullptr);
    MGR_EXPORT ~Manager();

    MGR_EXPORT void step();

    // These functions export Tensor objects that link the ECS
    // simulation state to the python bindings / PyTorch tensors (src/bindings.cpp)
    MGR_EXPORT madrona::py::Tensor resetTensor() const;
    MGR_EXPORT madrona::py::Tensor actionTensor() const;
    MGR_EXPORT madrona::py::Tensor rewardTensor() const;
    MGR_EXPORT madrona::py::Tensor doneTensor() const;
    MGR_EXPORT madrona::py::Tensor selfObservationTensor() const;
    MGR_EXPORT madrona::py::Tensor partnerObservationsTensor() const;
    MGR_EXPORT madrona::py::Tensor roomEntityObservationsTensor() const;
    MGR_EXPORT madrona::py::Tensor doorObservationTensor() const;
    MGR_EXPORT madrona::py::Tensor lidarTensor() const;
    MGR_EXPORT madrona::py::Tensor stepsRemainingTensor() const;
    MGR_EXPORT madrona::py::Tensor lavaObservationTensor() const;

    // These functions are used by the viewer to control the simulation
    // with keyboard inputs in place of DNN policy actions
    MGR_EXPORT void triggerReset(int32_t world_idx);
    MGR_EXPORT void setAction(int32_t world_idx,
                              int32_t agent_idx,
                              int32_t move_amount,
                              int32_t move_angle,
                              int32_t rotate,
                              int32_t grab);

private:
    struct Impl;
    struct CPUImpl;
    struct CUDAImpl;

    std::unique_ptr<Impl> impl_;
};

}

