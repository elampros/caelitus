#pragma once

/// @file
/// A reader/writer mutex that does not starve writers.
/// @ingroup cache

#include <pthread.h>

#include <system_error>

namespace caelitus::cache {

/// A shared mutex that lets a waiting writer in ahead of new readers.
///
/// std::shared_mutex on glibc prefers readers: under a steady stream of
/// overlapping reads a writer can wait forever. Read-mostly caches hit on
/// every incoming message see exactly that load, so writers (rare cache
/// updates) must not starve.
///
/// Not recursive: a thread holding the shared lock must not take it again
/// while a writer may be waiting (it would deadlock).
///
/// Satisfies the standard SharedMutex requirements: use it with
/// std::unique_lock and std::shared_lock. Linux (glibc) only.
class WriterPreferringMutex {
public:
    /// @throws std::system_error if the lock cannot be created.
    WriterPreferringMutex() {
        pthread_rwlockattr_t attr;
        pthread_rwlockattr_init(&attr);
        pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        const int rc = pthread_rwlock_init(&lock_, &attr);
        pthread_rwlockattr_destroy(&attr);
        if (rc != 0) throw std::system_error(rc, std::generic_category(), "pthread_rwlock_init");
    }
    ~WriterPreferringMutex() { pthread_rwlock_destroy(&lock_); }

    WriterPreferringMutex(const WriterPreferringMutex&) = delete;
    WriterPreferringMutex& operator=(const WriterPreferringMutex&) = delete;

    /// @name Exclusive (writer) locking
    /// What std::unique_lock calls. lock() waits; try_lock() returns false if busy.
    /// @{
    void lock() { check(pthread_rwlock_wrlock(&lock_)); }              ///< Waits for exclusive ownership.
    bool try_lock() { return pthread_rwlock_trywrlock(&lock_) == 0; }  ///< Exclusive ownership if free now.
    void unlock() { pthread_rwlock_unlock(&lock_); }                   ///< Releases exclusive ownership.
    /// @}

    /// @name Shared (reader) locking
    /// What std::shared_lock calls. Readers wait while a writer is waiting.
    /// @{
    void lock_shared() { check(pthread_rwlock_rdlock(&lock_)); }              ///< Waits for shared ownership.
    bool try_lock_shared() { return pthread_rwlock_tryrdlock(&lock_) == 0; }  ///< Shared ownership if possible now.
    void unlock_shared() { pthread_rwlock_unlock(&lock_); }                   ///< Releases shared ownership.
    /// @}

private:
    static void check(int rc) {
        if (rc != 0) throw std::system_error(rc, std::generic_category(), "pthread_rwlock");
    }

    pthread_rwlock_t lock_;
};

}  // namespace caelitus::cache
