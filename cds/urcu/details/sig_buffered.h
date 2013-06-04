/*
    This file is a part of libcds - Concurrent Data Structures library
    See http://libcds.sourceforge.net/

    (C) Copyright Maxim Khiszinsky (libcds.sf.com) 2006-2013
    Distributed under the BSD license (see accompanying file license.txt)

    Version 1.4.0
*/


#ifndef _CDS_URCU_DETAILS_SIG_BUFFERED_H
#define _CDS_URCU_DETAILS_SIG_BUFFERED_H

#include <cds/urcu/details/sh.h>
#ifdef CDS_URCU_SIGNAL_HANDLING_ENABLED

#include <cds/backoff_strategy.h>
#include <cds/container/vyukov_mpmc_cycle_queue.h>

#include <cds/details/std/mutex.h>

namespace cds { namespace urcu {

    /// User-space signal-handled RCU with deferred (buffered) reclamation
    /**
        @headerfile cds/urcu/signal_buffered.h

        This URCU implementation contains an internal buffer where retired objects are
        accumulated. When the buffer becomes full, the RCU \p synchronize function is called
        that waits until all reader/updater threads end up their read-side critical sections,
        i.e. until the RCU quiescent state will come. After that the buffer and all retired objects are freed.
        This synchronization cycle may be called in any thread that calls \ref retire_ptr function.

        The \p Buffer contains items of \ref retired_ptr type and it should support a queue interface with
        three function:
        - <tt> bool push( retired_ptr& p ) </tt> - places the retired pointer \p p into queue. If the function
            returns \p false it means that the buffer is full and RCU synchronization cycle must be processed.
        - <tt>bool pop( retired_ptr& p ) </tt> - pops queue's head item into \p p parameter; if the queue is empty
            this function must return \p false
        - <tt>size_t size()</tt> - returns queue's item count.

        The buffer is considered as full if \p push returns \p false or the buffer size reaches the RCU threshold.

        There is a wrapper \ref cds_urcu_signal_buffered_gc "gc<signal_buffered>" for \p %signal_buffered class
        that provides unified RCU interface. You should use this wrapper class instead \p %signal_buffered

        Template arguments:
        - \p Buffer - buffer type. Default is cds::container::VyukovMPMCCycleQueue
        - \p Lock - mutex type, default is \p std::mutex
        - \p Backoff - back-off schema, default is cds::backoff::Default
    */
    template <
        class Buffer = cds::container::VyukovMPMCCycleQueue<
            epoch_retired_ptr
            ,cds::opt::buffer< cds::opt::v::dynamic_buffer< epoch_retired_ptr > >
        >
        ,class Lock = cds_std::mutex
        ,class Backoff = cds::backoff::Default
    >
    class signal_buffered: public details::sh_singleton< signal_buffered_tag >
    {
        //@cond
        typedef details::sh_singleton< signal_buffered_tag > base_class ;
        //@endcond
    public:
        typedef signal_buffered_tag rcu_tag ;  ///< RCU tag
        typedef Buffer  buffer_type ;   ///< Buffer type
        typedef Lock    lock_type   ;   ///< Lock type
        typedef Backoff back_off    ;   ///< Back-off type

        typedef base_class::thread_gc thread_gc ;   ///< Thread-side RCU part
        typedef typename thread_gc::scoped_lock scoped_lock ; ///< Access lock class

        static bool const c_bBuffered = true ; ///< This RCU buffers disposed elements

    protected:
        //@cond
        typedef details::sh_singleton_instance< rcu_tag >    singleton_ptr ;
        //@endcond

    protected:
        //@cond
        buffer_type                     m_Buffer    ;
        CDS_ATOMIC::atomic<uint64_t>    m_nCurEpoch ;
        lock_type                       m_Lock      ;
        size_t const                    m_nCapacity ;
        //@endcond

    public:
        /// Returns singleton instance
        static signal_buffered * instance()
        {
            return static_cast<signal_buffered *>( base_class::instance() ) ;
        }
        /// Checks if the singleton is created and ready to use
        static bool isUsed()
        {
            return singleton_ptr::s_pRCU != null_ptr<singleton_vtbl *>() ;
        }

    protected:
        //@cond
        signal_buffered( size_t nBufferCapacity, int nSignal = SIGUSR1 )
            : base_class( nSignal )
            , m_Buffer( nBufferCapacity )
            , m_nCurEpoch(0)
            , m_nCapacity( nBufferCapacity )
        {}

        ~signal_buffered()
        {
            clear_buffer( (uint64_t) -1 ) ;
        }

        void clear_buffer( uint64_t nEpoch )
        {
            epoch_retired_ptr p ;
            while ( m_Buffer.pop( p )) {
                if ( p.m_nEpoch <= nEpoch )
                    p.free() ;
                else {
                    push_buffer( p );
                    break;
                }
            }
        }

        void push_buffer( epoch_retired_ptr& ep )
        {
            bool bPushed = m_Buffer.push( ep ) ;
            if ( !bPushed || m_Buffer.size() >= capacity() ) {
                if ( synchronize( ep ))
                    ep.free()    ;
            }
        }
        //@endcond

    public:
        /// Creates singleton object
        /**
            The \p nBufferCapacity parameter defines RCU threshold.

            The \p nSignal parameter defines a signal number stated for RCU, default is \p SIGUSR1
        */
        static void Construct( size_t nBufferCapacity = 256, int nSignal = SIGUSR1 )
        {
            if ( !singleton_ptr::s_pRCU )
                singleton_ptr::s_pRCU = new signal_buffered( nBufferCapacity, nSignal ) ;
        }

        /// Destroys singleton object
        static void Destruct( bool bDetachAll = false )
        {
            if ( isUsed() ) {
                instance()->clear_buffer( (uint64_t) -1 )  ;
                if ( bDetachAll )
                    instance()->m_ThreadList.detach_all() ;
                delete instance()   ;
                singleton_ptr::s_pRCU = null_ptr<singleton_vtbl *>() ;
            }
        }

    public:
        /// Retire \p p pointer
        /**
            The method pushes \p p pointer to internal buffer.
            When the buffer becomes full \ref synchronize function is called
            to wait for the end of grace period and then to free all pointers from the buffer.
        */
        virtual void retire_ptr( retired_ptr& p )
        {
            if ( p.m_p ) {
                epoch_retired_ptr ep( p, m_nCurEpoch.load( CDS_ATOMIC::memory_order_relaxed )) ;
                push_buffer( ep )   ;
            }
        }

        /// Wait to finish a grace period and then clear the buffer
        void synchronize()
        {
            epoch_retired_ptr ep( retired_ptr(), m_nCurEpoch.load( CDS_ATOMIC::memory_order_relaxed )) ;
            synchronize( ep ) ;
        }

        //@cond
        bool synchronize( epoch_retired_ptr& ep )
        {
            uint64_t nEpoch ;
            CDS_ATOMIC::atomic_thread_fence( CDS_ATOMIC::memory_order_acquire ) ;
            {
                cds::lock::scoped_lock<lock_type> sl( m_Lock )  ;
                if ( ep.m_p && m_Buffer.push( ep ))
                    return false    ;
                nEpoch = m_nCurEpoch.fetch_add( 1, CDS_ATOMIC::memory_order_relaxed ) ;

                back_off bkOff ;
                base_class::force_membar_all_threads( bkOff )   ;
                base_class::switch_next_epoch() ;
                bkOff.reset() ;
                base_class::wait_for_quiescent_state( bkOff )   ;
                base_class::switch_next_epoch() ;
                bkOff.reset() ;
                base_class::wait_for_quiescent_state( bkOff )   ;
                base_class::force_membar_all_threads( bkOff )   ;
            }

            clear_buffer( nEpoch )  ;
            return true ;
        }
        //@endcond

        /// Returns the threshold of internal buffer
        size_t capacity() const
        {
            return m_nCapacity ;
        }

        /// Returns the signal number stated for RCU
        int signal_no() const
        {
            return base_class::signal_no() ;
        }
    };

}} // namespace cds::urcu

#endif // #ifdef CDS_URCU_SIGNAL_HANDLING_ENABLED
#endif // #ifndef _CDS_URCU_DETAILS_SIG_BUFFERED_H