// Copyright (c) Lawrence Livermore National Security, LLC and other Conduit
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Conduit.

//-----------------------------------------------------------------------------
///
/// file: t_blueprint_mesh_examples.cpp
///
//-----------------------------------------------------------------------------

#include "conduit.hpp"
#include "conduit_blueprint.hpp"
#include "conduit_relay.hpp"
#include "conduit_log.hpp"

#include "conduit_fmt/conduit_fmt.h"

#include <math.h>
#include <iostream>
#include "gtest/gtest.h"

using namespace conduit;
using namespace conduit::utils;

//-----------------------------------------------------------------------------
TEST(conduit_blueprint_mesh_relay, spiral_multi_file)
{
    Node io_protos;
    relay::io::about(io_protos["io"]);
    bool hdf5_enabled = io_protos["io/protocols/hdf5"].as_string() == "enabled";
    if(!hdf5_enabled)
    {
        CONDUIT_INFO("HDF5 disabled, skipping spiral_multi_file test");
        return;
    }
    //
    // Create an example mesh.
    //
    Node data, verify_info;

    // use spiral , with 7 domains
    conduit::blueprint::mesh::examples::spiral(7,data);

    // lets try with -1 to 8 files.

    // nfiles less than 1 should trigger default case
    // (n output files = n domains)
    std::ostringstream oss;
    for(int nfiles=-1; nfiles < 9; nfiles++)
    {
        CONDUIT_INFO("test nfiles = " << nfiles);
        oss.str("");
        oss << "tout_relay_sprial_mesh_save_nfiles_" << nfiles;
        std::string output_base = oss.str();
        std::string output_dir  = output_base + ".cycle_000000";
        std::string output_root = output_base + ".cycle_000000.root";

        // count the files
        //  file_%06llu.{protocol}:/domain_%06llu/...
        int nfiles_to_check = nfiles;
        if(nfiles <=0 || nfiles == 8) // expect 7 files (one per domain)
        {
            nfiles_to_check = 7;
        }

        // remove existing root file, directory and any output files
        remove_path_if_exists(output_root);
        for(int i=0;i<nfiles_to_check;i++)
        {

            std::string fprefix = "file_";
            if(nfiles_to_check == 7)
            {
                // in the n domains == n files case, the file prefix is
                // domain_
                fprefix = "domain_";
            }

            std::string output_file = conduit_fmt::format("{}{:06d}.hdf5",
                            join_file_path(output_base + ".cycle_000000",
                                           fprefix),
                            i);
            remove_path_if_exists(output_file);
        }

        remove_path_if_exists(output_dir);

        Node opts;
        opts["number_of_files"] = nfiles;
        relay::io::blueprint::write_mesh(data, output_base, "hdf5", opts);

        EXPECT_TRUE(is_directory(output_dir));
        EXPECT_TRUE(is_file(output_root));

        char fmt_buff[64] = {0};
        for(int i=0;i<nfiles_to_check;i++)
        {

            std::string fprefix = "file_";
            if(nfiles_to_check == 7)
            {
                // in the n domains == n files case, the file prefix is
                // domain_
                fprefix = "domain_";
            }

            std::string fcheck = conduit_fmt::format("{}{:06d}.hdf5",
                            join_file_path(output_base + ".cycle_000000",
                                           fprefix),
                            i);

            std::cout << " checking: " << fcheck << std::endl;
            EXPECT_TRUE(is_file(fcheck));
        }

        // read the mesh back in diff to make sure we have the same data
        Node n_read, info;
        relay::io::blueprint::read_mesh(output_base + ".cycle_000000.root",
                                        n_read);

        // in all cases we expect 7 domains to match 
        for(int dom_idx =0; dom_idx <7; dom_idx++)
        {
            EXPECT_FALSE(data.child(dom_idx).diff(n_read.child(dom_idx),info));
        }
    }
}


//-----------------------------------------------------------------------------
TEST(conduit_blueprint_mesh_relay, save_read_mesh)
{
    Node io_protos;
    relay::io::about(io_protos["io"]);
    bool hdf5_enabled = io_protos["io/protocols/hdf5"].as_string() == "enabled";
    if(!hdf5_enabled)
    {
        CONDUIT_INFO("HDF5 disabled, skipping save_read_mesh test");
        return;
    }

    std::string output_base = "tout_relay_mesh_save_load";
    // spiral with 3 domains
    Node data;
    conduit::blueprint::mesh::examples::spiral(3,data);

    // spiral doesn't have domain ids, lets add some so we diff clean
    data.child(0)["state/domain_id"] = 0;
    data.child(1)["state/domain_id"] = 1;
    data.child(2)["state/domain_id"] = 2;

    Node opts;
    opts["number_of_files"] = -1;

    remove_path_if_exists(output_base + ".cycle_000000.root");
    remove_path_if_exists(output_base + ".cycle_000000/file_000000.hdf5");
    remove_path_if_exists(output_base + ".cycle_000000/file_000001.hdf5");
    remove_path_if_exists(output_base + ".cycle_000000/file_000002.hdf5");

    relay::io::blueprint::write_mesh(data, output_base, "hdf5", opts);

    data.print();
    Node n_read, info;
    relay::io::blueprint::read_mesh(output_base + ".cycle_000000.root",
                                    n_read);

    n_read.print();
    // reading back in will add domain_zzzzzz names, check children of read

    EXPECT_FALSE(data.child(0).diff(n_read.child(0),info));
    EXPECT_FALSE(data.child(1).diff(n_read.child(1),info));
    EXPECT_FALSE(data.child(2).diff(n_read.child(2),info));
}

//-----------------------------------------------------------------------------
TEST(conduit_blueprint_mesh_relay, save_read_mesh_truncate)
{
    Node io_protos;
    relay::io::about(io_protos["io"]);
    bool hdf5_enabled = io_protos["io/protocols/hdf5"].as_string() == "enabled";
    if(!hdf5_enabled)
    {
        CONDUIT_INFO("HDF5 disabled, skipping save_read_mesh_truncate test");
        return;
    }

    std::string output_base = "tout_relay_mesh_save_load_truncate";
    
    Node data;

    // 3 doms
    blueprint::mesh::examples::braid("uniform",
                                     2,
                                     2,
                                     2,
                                     data.append());

    blueprint::mesh::examples::braid("uniform",
                                     2,
                                     2,
                                     2,
                                     data.append());

    blueprint::mesh::examples::braid("uniform",
                                     2,
                                     2,
                                     2,
                                     data.append());

    // set the domain ids
    data.child(0)["state/domain_id"] = 0;
    data.child(1)["state/domain_id"] = 1;
    data.child(2)["state/domain_id"] = 2;

    // 3 doms 2 files
    Node opts;
    opts["number_of_files"] = 2;

    remove_path_if_exists(output_base + ".cycle_000100.root");
    remove_path_if_exists(output_base + ".cycle_000100/file_000000.hdf5");
    remove_path_if_exists(output_base + ".cycle_000100/file_000001.hdf5");

    relay::io::blueprint::write_mesh(data, output_base, "hdf5", opts);

    // load mesh back back in and diff to check values
    Node n_read, info;
    relay::io::blueprint::load_mesh(output_base + ".cycle_000100.root",
                                    n_read);

    for(int dom_idx=0; dom_idx < 3; dom_idx++)
    {
        data.child(dom_idx).diff(n_read.child(dom_idx),info);
    }

    // now save diff mesh to same file set, will fail with write_mesh
    // b/c hdf5 paths won't be compat

    data.reset();
    // 3 doms
    blueprint::mesh::examples::braid("uniform",
                                     5,
                                     5,
                                     0,
                                     data.append());

    blueprint::mesh::examples::braid("uniform",
                                     5,
                                     5,
                                     0,
                                     data.append());

    blueprint::mesh::examples::braid("uniform",
                                     5,
                                     5,
                                     0,
                                     data.append());

    // set the domain ids
    data.child(0)["state/domain_id"] = 0;
    data.child(1)["state/domain_id"] = 1;
    data.child(2)["state/domain_id"] = 2;

    // non-trunk will error b/c leaf sizes aren't compat
    EXPECT_THROW( relay::io::blueprint::write_mesh(data, output_base, "hdf5", opts),Error);

    // trunc will work
    relay::io::blueprint::save_mesh(data, output_base, "hdf5", opts);
    
    // load mesh back back in and diff to check values
    relay::io::blueprint::load_mesh(output_base + ".cycle_000100.root",
                                    n_read);

    for(int dom_idx=0; dom_idx < 3; dom_idx++)
    {
        EXPECT_FALSE(data.child(dom_idx).diff(n_read.child(dom_idx),info));
        info.print();
    }
}


//-----------------------------------------------------------------------------
TEST(conduit_blueprint_mesh_relay, save_read_mesh_truncate_root_only)
{
    // test truncate with root only style
    Node io_protos;
    relay::io::about(io_protos["io"]);
    bool hdf5_enabled = io_protos["io/protocols/hdf5"].as_string() == "enabled";
    if(!hdf5_enabled)
    {
        CONDUIT_INFO("HDF5 disabled, skipping save_read_mesh_truncate test");
        return;
    }
    

    std::string output_base = "tout_relay_mesh_save_load_truncate";
    
    Node data;

    blueprint::mesh::examples::braid("uniform",
                                     2,
                                     2,
                                     2,
                                     data);

    remove_path_if_exists(output_base + ".cycle_000100.root");

    Node opts;
    relay::io::blueprint::write_mesh(data, output_base, "hdf5", opts);

    blueprint::mesh::examples::braid("uniform",
                                     10,
                                     10,
                                     10,
                                     data);


    EXPECT_THROW( relay::io::blueprint::write_mesh(data, output_base, "hdf5", opts),Error);
    opts["truncate"] = "true";
    // this will succed
    relay::io::blueprint::write_mesh(data, output_base, "hdf5", opts);
    
}

//-----------------------------------------------------------------------------
TEST(conduit_blueprint_mesh_relay, save_read_mesh_opts)
{
    Node io_protos;
    relay::io::about(io_protos["io"]);
    bool hdf5_enabled = io_protos["io/protocols/hdf5"].as_string() == "enabled";
    if(!hdf5_enabled)
    {
        CONDUIT_INFO("HDF5 disabled, skipping save_read_mesh_opts test");
        return;
    }

    Node data;
    blueprint::mesh::examples::braid("uniform",
                                     2,
                                     2,
                                     2,
                                     data);

    // add a domain_id since one will come back
    data["state/domain_id"] = 0;

    //
    // suffix
    //

    // suffix: default, cycle, none, garbage

    std::string tout_base = "tout_relay_bp_mesh_opts_suffix";

    Node opts;
    Node n_read, info;
    opts["file_style"] = "root_only";

    //
    opts["suffix"] = "default";

    remove_path_if_exists(tout_base + ".cycle_000100.root");
    relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts);
    EXPECT_TRUE(is_file(tout_base + ".cycle_000100.root"));

    // load mesh back back in and diff to check values
    relay::io::blueprint::load_mesh(tout_base + ".cycle_000100.root",
                                    n_read);
    EXPECT_FALSE(data.diff(n_read.child(0),info));
    data.print();
    n_read.print();
    info.print();

    // remove cycle from braid, default behavior will be diff
    data.remove("state/cycle");

    remove_path_if_exists(tout_base + ".root");
    relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts);
    EXPECT_TRUE(is_file( tout_base + ".root"));

    // load mesh back back in and diff to check values
    relay::io::blueprint::load_mesh(tout_base + ".root",
                                    n_read);
    EXPECT_FALSE(data.diff(n_read.child(0),info));


    //
    opts["suffix"] = "cycle";

    remove_path_if_exists(tout_base + ".cycle_000000.root");
    relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts);
    EXPECT_TRUE(is_file( tout_base + ".cycle_000000.root"));

    relay::io::blueprint::load_mesh(tout_base + ".cycle_000000.root",
                                    n_read);
    EXPECT_FALSE(data.diff(n_read.child(0),info));

    //
    opts["suffix"] = "none";

    remove_path_if_exists(tout_base + ".root");
    relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts);
    EXPECT_TRUE(is_file( tout_base + ".root"));

    relay::io::blueprint::load_mesh(tout_base + ".root",
                                    n_read);
    EXPECT_FALSE(data.diff(n_read.child(0),info));

    // this should error
    opts["suffix"] = "garbage";
    EXPECT_THROW(relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts),Error);


    //
    // file style
    //
    // default, root_only, multi_file, garbage

    tout_base = "tout_relay_bp_mesh_opts_file_style";

    opts["file_style"] = "default";
    opts["suffix"] = "none";

    remove_path_if_exists(tout_base + ".root");
    relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts);
    EXPECT_TRUE(is_file( tout_base + ".root"));

    // load mesh back back in and diff to check values
    relay::io::blueprint::load_mesh(tout_base + ".root",
                                    n_read);
    EXPECT_FALSE(data.diff(n_read.child(0),info));

    opts["file_style"] = "root_only";

    remove_path_if_exists(tout_base + ".root");
    relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts);
    EXPECT_TRUE(is_file( tout_base + ".root"));

    // load mesh back back in and diff to check values
    relay::io::blueprint::load_mesh(tout_base + ".root",
                                    n_read);
    EXPECT_FALSE(data.diff(n_read.child(0),info));



    opts["file_style"] = "multi_file";

    remove_path_if_exists(tout_base + ".root");
    remove_path_if_exists(tout_base);
    relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts);
    EXPECT_TRUE(is_file( tout_base + ".root"));
    EXPECT_TRUE(is_directory(tout_base));
    EXPECT_TRUE(is_file(join_file_path(tout_base,
                                       "domain_000000.hdf5")));

    // load mesh back back in and diff to check values
    relay::io::blueprint::load_mesh(tout_base + ".root",
                                    n_read);
    EXPECT_FALSE(data.diff(n_read.child(0),info));


    opts["file_style"] = "garbage";

    EXPECT_THROW(relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts),Error);

    //
    // mesh name
    //

    opts["file_style"] = "default";
    opts["suffix"] = "none";
    opts["mesh_name"] = "bananas";

    tout_base = "tout_relay_bp_mesh_opts_mesh_name";
    remove_path_if_exists(tout_base + ".root");
    relay::io::blueprint::write_mesh(data, tout_base, "hdf5", opts);
    EXPECT_TRUE(is_file( tout_base + ".root"));

    // even custom name should work just fine with default
    // (it will pick the first mesh)
    relay::io::blueprint::load_mesh(tout_base + ".root", n_read);

    Node load_opts;
    // now test bad name
    load_opts["mesh_name"] = "garbage";
    EXPECT_THROW(relay::io::blueprint::load_mesh(tout_base + ".root",
                                                 load_opts,
                                                 n_read),
                 Error);

    // now test expected name
    load_opts["mesh_name"] = "bananas";
    relay::io::blueprint::load_mesh(tout_base + ".root", load_opts, n_read);
    
    // check that 
    // load mesh back back in and diff to check values
    relay::io::blueprint::load_mesh(tout_base + ".root",
                                    n_read);
    EXPECT_FALSE(data.diff(n_read.child(0),info));
}


