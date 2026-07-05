#pragma once

#include <vector>
#include <mutex>
#include <condition_variable>
#include <complex>
#include <cstdint>

// IQ Verisini temsil eder (In-phase & Quadrature)
using IQSample = std::complex<float>;

class CircularBuffer {
public:
    explicit CircularBuffer(size_t size) : buffer_(size), head_(0), tail_(0), full_(false) {}

    // Buffer'a veri ekler (Producer kullanacak)
    void push(const IQSample& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_[head_] = item;
        
        if (full_) {
            tail_ = (tail_ + 1) % buffer_.size(); // Eski veri üzerine yazılıyor
        }
        
        head_ = (head_ + 1) % buffer_.size();
        full_ = head_ == tail_;
        
        // Consumer'ı uyar
        cond_.notify_one();
    }

    // Buffer'dan veri okur (Consumer kullanacak)
    bool pop(IQSample& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Eğer buffer boşsa bekle
        cond_.wait(lock, [this]() { return full_ || (head_ != tail_); });

        item = buffer_[tail_];
        full_ = false;
        tail_ = (tail_ + 1) % buffer_.size();
        
        return true;
    }

    // Buffer'ın o anki doluluk oranını döndürür
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t size = buffer_.size();
        if (full_) return size;
        if (head_ >= tail_) return head_ - tail_;
        return size + head_ - tail_;
    }

    // Buffer'ın toplam kapasitesini döndürür
    size_t capacity() const {
        return buffer_.size();
    }

private:
    std::vector<IQSample> buffer_;
    size_t head_;
    size_t tail_;
    bool full_;
    
    mutable std::mutex mutex_;
    std::condition_variable cond_;
};
