// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Vulkan {

class PipelineCompileQueue {
public:
    using Task = std::function<void()>;

    explicit PipelineCompileQueue(u32 worker_count = 1);
    ~PipelineCompileQueue();

    void Start();
    void Stop();
    bool Enqueue(Task task);

    [[nodiscard]] u32 QueueDepth() const;
    [[nodiscard]] u64 CompletedTasks() const;

private:
    void WorkerLoop();

private:
    u32 worker_count_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<u32> queue_depth_{0};
    std::atomic<u64> completed_{0};
};

} // namespace Vulkan
