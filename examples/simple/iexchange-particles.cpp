#include <vector>
#include <iostream>

#include <diy/mpi.hpp>
#include <diy/master.hpp>
#include <diy/assigner.hpp>
#include <diy/serialization.hpp>

struct Block
{
    Block(int c = 0): count(c)              {}

    int   count;
};

struct Particle
{
    Particle(int id_ = -1, int h = 0):
        id(id_), hops(h)                    {}

    int   id;
    int   hops;
};

void* create_block()                        { return new Block; }
void  destroy_block(void*           b)      { delete static_cast<Block*>(b); }
void  save_block(const void*        b,
                 diy::BinaryBuffer& bb)     { diy::save(bb, *static_cast<const Block*>(b)); }
void  load_block(void*              b,
                 diy::BinaryBuffer& bb)     { diy::load(bb, *static_cast<Block*>(b)); }


// callback for asynchronous iexchange
// return: true = I'm done unless more work arrives; false = I'm not done, please call me again
bool foo(Block*                             b,
        const diy::Master::IProxyWithLink&  icp)
{
    diy::Link* l = icp.link();
    int my_gid   = icp.gid();

    //fmt::print(stderr, "Block {} with {} count\n", my_gid, b->count);

    // start with every block enqueueing particles to random neighbors
    int id = my_gid * 1000;
    while (b->count > 0)
    {
        int nbr = rand() % l->size();
        Particle p(id++, 1 + rand() % 20);
        icp.enqueue(l->target(nbr), p);
        b->count--;
        fmt::print(stderr, "enq [{}] -> ({},{}) -> [{}]\n", my_gid, p.id, p.hops, l->target(nbr).gid);
    }

    // then dequeue as long as something is incoming and enqueue as long as the hop count is not exceeded
    // foo will be called by master multiple times until no more messages are in flight anywhere
    for (size_t i = 0; i < l->size(); ++i)
    {
        int nbr_gid = l->target(i).gid;
        if (icp.incoming(nbr_gid))      // FIXME: make this while
        {
            Particle p;
            icp.dequeue(nbr_gid, p);
            fmt::print(stderr, "deq [{}] <- ({},{}) <- [{}]\n", my_gid, p.id, p.hops, nbr_gid);

            p.hops--;
            if (p.hops > 0)
            {
                int nbr = rand() % l->size();
                icp.enqueue(l->target(nbr), p);
                fmt::print(stderr, "enq [{}] -> ({},{}) -> [{}]\n", my_gid, p.id, p.hops, l->target(nbr).gid);
            }
        }
    }

    return (b->count == 0);      // this will always be true, but the logic is that we have no work left inside the block
}

int main(int argc, char* argv[])
{
//     diy::create_logger("trace");

    diy::mpi::environment     env(argc, argv);
    diy::mpi::communicator    world;

    int                       nblocks = 2 * world.size();

    diy::FileStorage          storage("./DIY.XXXXXX");

    diy::Master               master(world,
                                     1,
                                     -1,
                                     &create_block,
                                     &destroy_block,
                                     &storage,
                                     &save_block,
                                     &load_block);

    srand(time(NULL) + world.rank());

    diy::RoundRobinAssigner   assigner(world.size(), nblocks);

    // this example creates a linear chain of blocks
    std::vector<int> gids;                     // global ids of local blocks
    assigner.local_gids(world.rank(), gids);   // get the gids of local blocks
    for (size_t i = 0; i < gids.size(); ++i)   // for the local blocks in this processor
    {
        int gid = gids[i];

        diy::Link*   link = new diy::Link;   // link is this block's neighborhood
        diy::BlockID neighbor;               // one neighbor in the neighborhood
        if (gid < nblocks - 1)               // all but the last block in the global domain
        {
            neighbor.gid  = gid + 1;                     // gid of the neighbor block
            neighbor.proc = assigner.rank(neighbor.gid); // process of the neighbor block
            link->add_neighbor(neighbor);                // add the neighbor block to the link
        }
        if (gid > 0)                         // all but the first block in the global domain
        {
            neighbor.gid  = gid - 1;
            neighbor.proc = assigner.rank(neighbor.gid);
            link->add_neighbor(neighbor);
        }

        master.add(gid, new Block(1 + rand() % 10), link);    // add the current local block to the master
    }

#if 0
    // test synchronous version
    master.foreach(&enq);
    master.exchange();
    master.foreach(&deq);
#else
    // dequeue, enqueue, exchange all in one nonblocking routine
    master.iexchange(&foo);
#endif

    if (world.rank() == 0)
        fmt::print(stderr, "Total iterations: {}\n", master.block<Block>(master.loaded_block())->count);
}
