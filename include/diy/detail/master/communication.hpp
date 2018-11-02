namespace diy
{
    struct Master::tags             { enum { queue, piece }; };

    struct Master::MessageInfo
    {
        int from, to;
        int round;
    };

    struct Master::InFlightSend
    {
        std::shared_ptr<MemoryBuffer> message;
        mpi::request                  request;

        MessageInfo info;           // for debug purposes
    };

    struct Master::InFlightRecv
    {
        MemoryBuffer    message;
        MessageInfo     info { -1, -1, -1 };
        bool            done = false;

        inline void     recv(mpi::communicator& comm, const mpi::status& status);
        inline void     place(IncomingRound* in, bool unload, ExternalStorage* storage, IExchangeInfo* iexchange);
        void            reset()     { *this = InFlightRecv(); }
    };

    struct Master::InFlightRecvsMap: public std::map<int, InFlightRecv>
    {};

    struct Master::InFlightSendsList: public std::list<InFlightSend>
    {};

    struct Master::GidSendOrder
    {
        size_t              size() const                        { return list.size(); }
        bool                empty() const                       { return list.empty(); }
        int                 pop()                               { int x = list.front(); list.pop_front(); return x; }

        std::list<int>      list;
        size_t              limit = 0;
    };

    struct Master::IExchangeInfo
    {
        using   Clock   = std::chrono::high_resolution_clock;
        using   Time    = Clock::time_point;

                        IExchangeInfo():
                            n(0),
                            min_queue_size_(0),
                            max_hold_time_(0),
                            gid(-1)                             {}
                        IExchangeInfo(size_t n_, mpi::communicator comm_, size_t min_queue_size, size_t max_hold_time):
                            n(n_),
                            comm(comm_),
                            global_work_(new mpi::window<int>(comm, 1)),
                            min_queue_size_(min_queue_size),
                            max_hold_time_(max_hold_time),
                            gid(-1)                             { global_work_->lock_all(MPI_MODE_NOCHECK); time_stamp_send(); }
                        ~IExchangeInfo()                        { global_work_->unlock_all(); }

      inline void       not_done(int gid);

      inline int        global_work();                          // get global work status (for debugging)
      inline bool       all_done();                             // get global all done status
      inline void       reset_work();                           // reset global work counter
      inline int        add_work(int work);                     // add work to global work counter
      int               inc_work()                              { return add_work(1); }   // increment global work counter
      int               dec_work()                              { return add_work(-1); }  // decremnent global work counter
      void              time_stamp_send()                       { time_last_send = Clock::now(); }
      bool              hold(size_t queue_size)                 { return min_queue_size_ >= 0 && max_hold_time_ >= 0 &&
                                                                         queue_size < min_queue_size_ && hold_time() < max_hold_time_; }
      size_t            hold_time()                             { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - time_last_send).count(); }
      bool              shortcut()                              { return gid >= 0; }
      void              clear_shortcut()                        { gid = -1; }
      void              set_shortcut(int gid_,
                                     BlockID to_block)          { gid = gid_; block_id = to_block; }



      size_t                              n;
      mpi::communicator                   comm;
      std::unordered_map<int, bool>       done;                 // gid -> done
      std::unique_ptr<mpi::window<int>>   global_work_;         // global work to do
      std::shared_ptr<spd::logger>        log = get_logger();
      Time                                time_last_send;       // time of last send from any queue in send_outgoing_queues()

      // TODO: for now, negative min_queue_size or max_hold_time indicates don't do fine-grain icommunicate at all
      int                                 min_queue_size_;      // minimum short message size (bytes)
      int                                 max_hold_time_;       // maximum short message hold time (milliseconds)

      // the following 2 members are for short-cutting send_outgoing_queues
      // gid = -1 means ignore gid and block_id and don't shortcut send_outgoing_queues
      int                                 gid;                  // gid of most recent enqueue
      BlockID                             block_id;             // block id of target of most recent enqueue
    };

    // VectorWindow is used to send and receive subsets of a contiguous array in-place
    namespace detail
    {
        template <typename T>
        struct VectorWindow
        {
            T *begin;
            size_t count;
        };
    } // namespace detail

    namespace mpi
    {
    namespace detail
    {
        template<typename T>  struct is_mpi_datatype< diy::detail::VectorWindow<T> > { typedef true_type type; };

        template <typename T>
        struct mpi_datatype< diy::detail::VectorWindow<T> >
        {
            using VecWin = diy::detail::VectorWindow<T>;
            static MPI_Datatype         datatype()                { return get_mpi_datatype<T>(); }
            static const void*          address(const VecWin& x)  { return x.begin; }
            static void*                address(VecWin& x)        { return x.begin; }
            static int                  count(const VecWin& x)    { return static_cast<int>(x.count); }
        };
    }
    } // namespace mpi::detail
} // namespace diy

void
diy::Master::IExchangeInfo::
not_done(int gid)
{
    if (done[gid])
    {
        done[gid] = false;
        int work = inc_work();
        log->debug("[{}] Incrementing work when switching done (on receipt): work = {}\n", gid, work);
    } else
        log->debug("[{}] Not done, no need to increment work\n", gid);
}

diy::Master::InFlightRecv&
diy::Master::
inflight_recv(int proc)
{
    return (*inflight_recvs_)[proc];
}

diy::Master::InFlightSendsList&
diy::Master::inflight_sends()
{
    return *inflight_sends_;
}

// receive message described by status
void
diy::Master::InFlightRecv::
recv(mpi::communicator& comm, const mpi::status& status)
{
    if (info.from == -1)            // uninitialized
    {
        MemoryBuffer bb;
        comm.recv(status.source(), status.tag(), bb.buffer);

        if (status.tag() == tags::piece)     // first piece is the header
        {
            size_t msg_size;
            diy::load(bb, msg_size);
            diy::load(bb, info);

            message.buffer.reserve(msg_size);
        }
        else    // tags::queue
        {
            diy::load_back(bb, info);
            message.swap(bb);
        }
    }
    else
    {
        size_t start_idx = message.buffer.size();
        size_t count = status.count<char>();
        message.buffer.resize(start_idx + count);

        detail::VectorWindow<char> window;
        window.begin = &message.buffer[start_idx];
        window.count = count;

        comm.recv(status.source(), status.tag(), window);
    }

    if (status.tag() == tags::queue)
        done = true;
}

// once the InFlightRecv is done, place it either out of core or in the appropriate incoming queue
void
diy::Master::InFlightRecv::
place(IncomingRound* in, bool unload, ExternalStorage* storage, IExchangeInfo* iexchange)
{
    size_t size     = message.size();
    int from        = info.from;
    int to          = info.to;
    int external    = -1;

    if (unload)
    {
        get_logger()->debug("Directly unloading queue {} <- {}", to, from);
        external = storage->put(message);       // unload directly
    }
    else if (!iexchange)
    {
        in->map[to].queues[from].swap(message);
        in->map[to].queues[from].reset();       // buffer position = 0
    }
    else    // iexchange
    {
        auto log = get_logger();

        iexchange->not_done(to);
        in->map[to].queues[from].append_binary(&message.buffer[0], message.size());        // append insted of overwrite

        int work = iexchange->dec_work();
        log->debug("[{}] Decrementing work after receiving: work = {}\n", to, work);
    }
    in->map[to].records[from] = QueueRecord(size, external);

    ++(in->received);
}
