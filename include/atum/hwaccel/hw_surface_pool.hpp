#pragma once

#include <vector>
#include <queue>
#include <cstdint>
#include <mutex>
#include <memory>
#include <functional>
#include "atum/core/media_types.hpp"
#include "atum/surface/video_frame.hpp"
#include "atum/hwaccel/vaapi_device.hpp"

namespace atum {

inline constexpr uint32_t HW_INVALID_SURFACE_ID = 0xFFFFFFFF;

/// Sovereign Hardware Surface Pool for zero-allocation GPU memory surface recycling & lease tracking.
class HwSurfacePool {
public:
    struct ManagedSurface {
        uint32_t surface_id{HW_INVALID_SURFACE_ID};
        std::shared_ptr<void> lease;
    };

    HwSurfacePool() = default;
    ~HwSurfacePool() {
        release();
    }

    HwSurfacePool(const HwSurfacePool&) = delete;
    HwSurfacePool& operator=(const HwSurfacePool&) = delete;

    HwSurfacePool(HwSurfacePool&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        device_ = other.device_;
        surfaces_ = std::move(other.surfaces_);
        available_ = std::move(other.available_);
        slots_ = std::move(other.slots_);
        cursor_ = other.cursor_;
        other.device_ = nullptr;
        other.cursor_ = 0;
    }

    HwSurfacePool& operator=(HwSurfacePool&& other) noexcept {
        if (this != &other) {
            release();
            std::scoped_lock lock(mutex_, other.mutex_);
            device_ = other.device_;
            surfaces_ = std::move(other.surfaces_);
            available_ = std::move(other.available_);
            slots_ = std::move(other.slots_);
            cursor_ = other.cursor_;
            other.device_ = nullptr;
            other.cursor_ = 0;
        }
        return *this;
    }

    MediaResult<void> init(VaapiDevice* device, uint32_t width, uint32_t height, PixelFormat format, size_t pool_size) {
        if (!device || pool_size == 0) return MediaError::InvalidData;
        release();

        device_ = device;
        auto surf_res = device_->create_surfaces(width, height, format, static_cast<uint32_t>(pool_size));
        if (!surf_res.is_ok()) return surf_res.error();

        std::lock_guard<std::mutex> lock(mutex_);
        surfaces_ = std::move(*surf_res);
        while (!available_.empty()) available_.pop();
        slots_.clear();
        slots_.reserve(surfaces_.size());
        for (uint32_t s : surfaces_) {
            available_.push(s);
            Slot slot{};
            slot.surface_id = s;
            slots_.push_back(std::move(slot));
        }
        cursor_ = 0;
        return MediaResult<void>::ok();
    }

    void release() {
        VaapiDevice* dev_to_release = nullptr;
        std::vector<uint32_t> surfs_to_destroy;
        std::vector<std::shared_ptr<HwFrameBacking>> live_backings;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& slot : slots_) {
                if (auto backing = slot.active_backing.lock()) {
                    live_backings.push_back(std::move(backing));
                }
            }
            dev_to_release = device_;
            surfs_to_destroy = std::move(surfaces_);
            surfaces_.clear();
            slots_.clear();
            while (!available_.empty()) available_.pop();
            device_ = nullptr;
            cursor_ = 0;
        }

        // Preserve any external live VideoFrame pixels before destroying VA surfaces.
        for (auto& backing : live_backings) {
            backing->detach_surface_to_cpu();
        }
        if (dev_to_release && !surfs_to_destroy.empty()) {
            (void)dev_to_release->destroy_surfaces(surfs_to_destroy);
        }
    }

    /// Acquires a free surface ID wrapped in a shared lease; spills oldest non-DPB backing to CPU if all slots are held.
    template <typename InUsePred>
    ManagedSurface acquire_lease(InUsePred&& in_dpb_or_protected) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::shared_ptr<HwFrameBacking> spill_backing;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const size_t n = slots_.size();
                if (n == 0) return {};

                // Pass 1: find an unused slot (no live lease and not referenced in DPB)
                for (size_t k = 0; k < n; ++k) {
                    size_t idx = (cursor_ + k) % n;
                    Slot& slot = slots_[idx];
                    if (slot.active_lease.expired() && !in_dpb_or_protected(slot.surface_id)) {
                        auto lease = std::make_shared<uint8_t>(1);
                        slot.active_lease = lease;
                        slot.active_backing.reset();
                        cursor_ = (idx + 1) % n;
                        return {slot.surface_id, std::move(lease)};
                    }
                }

                // Pass 2: all slots leased by external frames; pick one not in DPB and spill it to CPU
                for (size_t k = 0; k < n; ++k) {
                    size_t idx = (cursor_ + k) % n;
                    Slot& slot = slots_[idx];
                    if (!in_dpb_or_protected(slot.surface_id)) {
                        if (auto b = slot.active_backing.lock()) {
                            spill_backing = std::move(b);
                            cursor_ = (idx + 1) % n;
                            break;
                        }
                        auto lease = std::make_shared<uint8_t>(1);
                        slot.active_lease = lease;
                        slot.active_backing.reset();
                        cursor_ = (idx + 1) % n;
                        return {slot.surface_id, std::move(lease)};
                    }
                }
            }

            if (spill_backing) {
                spill_backing->detach_surface_to_cpu();
            } else {
                break;
            }
        }
        return {};
    }

    void bind_backing(uint32_t surface_id, const std::shared_ptr<HwFrameBacking>& backing) noexcept {
        if (surface_id == HW_INVALID_SURFACE_ID || !backing) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot : slots_) {
            if (slot.surface_id == surface_id) {
                slot.active_backing = backing;
                return;
            }
        }
    }

    uint32_t acquire_surface() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (available_.empty()) return HW_INVALID_SURFACE_ID;
        uint32_t s = available_.front();
        available_.pop();
        return s;
    }

    void return_surface(uint32_t surface_id) {
        if (surface_id == HW_INVALID_SURFACE_ID) return;
        std::lock_guard<std::mutex> lock(mutex_);
        available_.push(surface_id);
    }

    [[nodiscard]] const std::vector<uint32_t>& all_surfaces() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return surfaces_;
    }

    [[nodiscard]] size_t available_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t free_slots = 0;
        if (!slots_.empty()) {
            for (const auto& s : slots_) {
                if (s.active_lease.expired()) ++free_slots;
            }
            return free_slots;
        }
        return available_.size();
    }

    [[nodiscard]] size_t total_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return surfaces_.size();
    }

    [[nodiscard]] bool is_valid() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return !surfaces_.empty();
    }

private:
    struct Slot {
        uint32_t surface_id{HW_INVALID_SURFACE_ID};
        std::weak_ptr<void> active_lease;
        std::weak_ptr<HwFrameBacking> active_backing;
    };

    VaapiDevice* device_{nullptr};
    std::vector<uint32_t> surfaces_;
    std::queue<uint32_t> available_;
    std::vector<Slot> slots_;
    size_t cursor_{0};
    mutable std::mutex mutex_;
};

} // namespace atum
