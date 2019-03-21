namespace diy
{
    struct Master::IExchangeInfo
    {
      using   Clock   = std::chrono::high_resolution_clock;
      using   Time    = Clock::time_point;

                        IExchangeInfo(mpi::communicator comm_, bool fine, size_t min_queue_size, size_t max_hold_time):
                            comm(comm_),
                            fine_(fine),
                            min_queue_size_(min_queue_size),
                            max_hold_time_(max_hold_time)   {}
      virtual           ~IExchangeInfo()                    {}

      virtual void      not_done(int gid)  =0;
      virtual void      update_done(int gid, bool done_) =0;

      virtual bool      all_done() =0;                             // get global all done status
      virtual void      add_work(int work) =0;                     // add work to global work counter
      virtual void      control() =0;
      virtual double    consensus_start_time() =0;

      void              inc_work()                              { add_work(1); }   // increment work counter
      void              dec_work()                              { add_work(-1); }  // decremnent work counter

      // shortcut
      void              time_stamp_send()                       { time_last_send = Clock::now(); }
      bool              hold(size_t queue_size)                 { return min_queue_size_ >= 0 && max_hold_time_ >= 0 &&
                                                                         queue_size < min_queue_size_ && hold_time() < max_hold_time_; }
      size_t            hold_time()                             { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - time_last_send).count(); }
      bool              fine() const                            { return fine_; }

      mpi::communicator                   comm;
      bool                                fine_ = false;

      std::shared_ptr<spd::logger>        log = get_logger();
      Time                                time_last_send;       // time of last send from any queue in send_outgoing_queues()

      int                                 min_queue_size_;      // minimum short message size (bytes)
      int                                 max_hold_time_;       // maximum short message hold time (milliseconds)

      int                                 from_gid = -1;        // gid of current block, for shortcut sending of only this block's queues
    };
}

#include "iexchange-dud.hpp"
#include "iexchange-collective.hpp"
