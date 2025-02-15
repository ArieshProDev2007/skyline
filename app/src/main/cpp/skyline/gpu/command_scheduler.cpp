// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include <loader/loader.h>
#include "command_scheduler.h"

namespace skyline::gpu {
    void CommandScheduler::WaiterThread() {
        if (int result{pthread_setname_np(pthread_self(), "Sky-CycleWaiter")})
            Logger::Warn("Failed to set the thread name: {}", strerror(result));

        try {
            signal::SetSignalHandler({SIGINT, SIGILL, SIGTRAP, SIGBUS, SIGFPE, SIGSEGV}, signal::ExceptionalSignalHandler);

            cycleQueue.Process([](const std::shared_ptr<FenceCycle> &cycle) {
                cycle->Wait(true);
            }, [] {});
        } catch (const signal::SignalException &e) {
            Logger::Error("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
            if (state.process)
                state.process->Kill(false);
            else
                std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            Logger::Error(e.what());
            if (state.process)
                state.process->Kill(false);
            else
                std::rethrow_exception(std::current_exception());
        }
    }

    CommandScheduler::CommandBufferSlot::CommandBufferSlot(vk::raii::Device &device, vk::CommandBuffer commandBuffer, vk::raii::CommandPool &pool)
        : device(device),
          commandBuffer(device, static_cast<VkCommandBuffer>(commandBuffer), static_cast<VkCommandPool>(*pool)),
          fence(device, vk::FenceCreateInfo{}),
          cycle(std::make_shared<FenceCycle>(device, *fence)) {}

    CommandScheduler::CommandScheduler(const DeviceState &state, GPU &pGpu)
        : state{state},
          gpu{pGpu},
          waiterThread{&CommandScheduler::WaiterThread, this},
          pool{std::ref(pGpu.vkDevice), vk::CommandPoolCreateInfo{
              .flags = vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
              .queueFamilyIndex = pGpu.vkQueueFamilyIndex,
          }} {}

    CommandScheduler::~CommandScheduler() {
        waiterThread.join();
    }

    CommandScheduler::ActiveCommandBuffer CommandScheduler::AllocateCommandBuffer() {
        for (auto &slot : pool->buffers) {
            if (!slot.active.test_and_set(std::memory_order_acq_rel)) {
                if (slot.cycle->Poll()) {
                    slot.commandBuffer.reset();
                    slot.cycle = std::make_shared<FenceCycle>(slot.device, *slot.fence);
                    return {slot};
                } else {
                    slot.active.clear(std::memory_order_release);
                }
            }
        }

        vk::CommandBuffer commandBuffer;
        vk::CommandBufferAllocateInfo commandBufferAllocateInfo{
            .commandPool = *pool->vkCommandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };

        auto result{(*gpu.vkDevice).allocateCommandBuffers(&commandBufferAllocateInfo, &commandBuffer, *gpu.vkDevice.getDispatcher())};
        if (result != vk::Result::eSuccess)
            vk::throwResultException(result, __builtin_FUNCTION());
        return {pool->buffers.emplace_back(gpu.vkDevice, commandBuffer, pool->vkCommandPool)};
    }

    void CommandScheduler::SubmitCommandBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &cycle) {
        {
            std::scoped_lock lock(gpu.queueMutex);
            gpu.vkQueue.submit(vk::SubmitInfo{
                .commandBufferCount = 1,
                .pCommandBuffers = &*commandBuffer,
            }, cycle->fence);
        }

        cycleQueue.Push(cycle);
    }
}
