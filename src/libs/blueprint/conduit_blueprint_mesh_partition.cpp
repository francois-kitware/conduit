// Copyright (c) Lawrence Livermore National Security, LLC and other Conduit
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Conduit.

//-----------------------------------------------------------------------------
///
/// file: conduit_blueprint_mesh_partition.cpp
///
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// conduit lib includes
//-----------------------------------------------------------------------------
#include "conduit_blueprint_mesh_partition.hpp"

//-----------------------------------------------------------------------------
// std lib includes
//-----------------------------------------------------------------------------
#include <algorithm>
#include <deque>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

//-----------------------------------------------------------------------------
// conduit includes
//-----------------------------------------------------------------------------
#include "conduit_blueprint_mcarray.hpp"
#include "conduit_blueprint_o2mrelation.hpp"
#include "conduit_blueprint_mesh_utils.hpp"
#include "conduit_blueprint_mesh.hpp"
#include "conduit_log.hpp"

//#ifdef CONDUIT_PARALLEL_PARTITION
//#include <mpi.h>
//#endif

using index_t=conduit::index_t;

extern void grid_ijk_to_id(const index_t *ijk, const index_t *dims, index_t &grid_id);
extern void grid_id_to_ijk(const index_t id, const index_t *dims, index_t *grid_ijk);

//-----------------------------------------------------------------------------
// -- begin conduit --
//-----------------------------------------------------------------------------
namespace conduit
{

//-----------------------------------------------------------------------------
// -- begin conduit::blueprint --
//-----------------------------------------------------------------------------
namespace blueprint
{

//-----------------------------------------------------------------------------
// -- begin conduit::blueprint::mesh --
//-----------------------------------------------------------------------------
namespace mesh
{

//-------------------------------------------------------------------------
/**
  options["comm"] = MPI_COMM_WORLD;

  "Information on how to re-decompose the grids will be provided or be easily calculatable."

  To go from 1-N, we need a way to pull the mesh apart. We would be able to
  provide the ways that this is done to select the bits we want so each selection
  makes a piece in the output mesh.

# Selections provide criteria used to split input meshes
options:
  selections:
    -
     type: "logical"
     domain: 0
     start: [0,0,0]
     end:   [10,10,10]
    -
     type: "logical"
     domain: 1
# We might have to specify a topo name too if there are multiple topos in the mesh.
# topology: "topo2"
     start: [50,50,50]
     end:   [60,60,60]
    -
     type: "explicit"
     domain: 0
     indices: [0,1,2,3,4,5,6,7,8,9]
    -
     type: "ranges"
     domain: 0
     ranges: [0,100, 1000,2000]
    -
     type: "explicit"
     domain: 1
     global_indices: [100,101,102,110,116,119,220]
    -
     type: "spatial"
     domain: 2
     box: [0., 0., 0., 11., 12., 22.]
    -
     type: "spatial"
     domain: 2
     box: [0., 0., 22., 11., 12., 44.]
  target: 7
  mapping: true
  replicate: false
  merging:
    enabled: true
    radius: 0.001
  fill_value: 0.0  # fill value to use for when not all chunks entirely cover space.
  root: 0
  mpi_comm: 11223


  If selections re not present in the options then we assume that we're selecting
  all cells in all domains that we passed. This is fine for N-1. For 1-N if we 
  do not pass selections then in serial, we're passing back the input mesh. In parallel,
  we could be doing that, or gathering to root rank.


  // For structured grids, select logically...
  options["selections/logical/0/domain"] = 0;
  options["selections/logical/0/start"] = {0,0,0};
  options["selections/logical/0/end"] = {10,10,10};
  options["selections/logical/1/domain"] = 1;
  options["selections/logical/1/start"] = {50,50,50};
  options["selections/logical/1/end"] = {60,60,60};

  // For any grid type, we could select explicit

  options["selections/cells/0/domain"] = 0;
  options["selections/cells/0/indices"] = {0,1,2,3,4,5,6,7,8,9};

  options["selections/cells/0/ranges"] = {0,100, 1000,2000};

  options["selections/cells/1/global_indices"] = {100,101,102,110,116,119,220};

  // Pull the mesh apart using spatial boxes

  options["selections/spatial/0/domain"] = 0;
  options["selections/spatial/0/box"] = {0., 0., 0., 11., 12., 22.};

  options["target"] = 1;        // The target number of domains across all ranks
                                // participating in the partition. If we have 1
                                // rank, 4 selections, and target=2, make 2 domains
                                // out of the 4 selected chunks.

  options["mapping"] = true;    // If on, preserve original cells, conduit::Node ids 
                                // so it is known how the output mesh was created.

  options["merging/enabled"] = false;
  options["merging/radius"] = 0.0001; // Point merging radius


  options["replicate"] = false; // If we're moving N-1 then indicate whether we
                                // want the data to be replicated on all ranks

  options["root"] = 0;  // Indicate which is the root rank if we're gathering data N-1.
  options["mpi_comm"] = integer representing MPI comm.


  Suppose we're in parallel on 100 ranks and each rank has 1 domain. Then say that we are
  making 10 target domains. Which ranks get them? The first 10 ranks in the comm? Does this
  need to be an option? dest_ranks=[10,20,30,40,...]?
*/
#if 0
size_t
determine_highest_topology(const std::vector<const conduit::Node *> &domains)
{
    size_t retval = 0; // points
    for(size_t i = 0; i < domains.size(); i++)
    {
        auto mt = domains[i]["topologies/mesh/type"].as_string();
        for(size_t j = 1; j < utils::TOPO_TYPES.size(); j++)
        {
            if(utils::TOPO_TYPES[j] == mt && j > retval)
            {
                retval = j;
                break;
            }
        }
    }
    return retval;
}
#endif

//---------------------------------------------------------------------------
/**
 @brief Base class for selections that identify regions of interest that will
        be extracted from a mesh.
 */
class selection
{
public:
    selection() : n_options_ptr(nullptr) { }
    virtual ~selection() { }

    virtual bool init(const conduit::Node *n_opt_ptr) = 0;

    /**
     @brief Determines whether the selection can be applied to the supplied mesh.
     @param n_mesh A Conduit conduit::Node containing the mesh.
     @return True if the selection can be applied to the mesh; False otherwise.
     */
    virtual bool applicable(const conduit::Node &n_mesh) = 0;

    /**
     @brief Return the number of cells in the selection.
     @return The number of cells in the selection.
     */
    virtual index_t length() const { return 0; }

    /**
     @brief Partitions the selection into smaller selections.
     @param n_mesh A Conduit conduit::Node containing the mesh.
     @return A vector of selection pointers that cover the input selection.
     */
    virtual std::vector<std::shared_ptr<selection> > partition(const conduit::Node &n_mesh) const = 0;

    /**
     @brief Return the domain index to which the selection is being applied.
     @return The domain index or 0. This value is 0 by default.
     */
    index_t get_domain() const
    {
        index_t n = 0;
        if(n_options_ptr && n_options_ptr->has_child(DOMAIN_KEY))
            n = n_options_ptr->child(DOMAIN_KEY).to_index_t();
        return n;
    }

    /**
     @brief Return whether element and vertex mapping will be preserved in the output.
     @return True if mapping information is preserved, false otherwise.
     */
    bool preserve_mapping() const
    {
        bool mapping = true;
        if(n_options_ptr && n_options_ptr->has_child(MAPPING_KEY))
            mapping = n_options_ptr->child(MAPPING_KEY).as_uint32() != 0;
        return mapping;
    }

    /**
     @brief Returns the cells in this selection that are contained in the
            supplied topology. Such cells will have cell ranges in erange,
            inclusive. The element ids are returned in element_ids.
     */
    virtual void get_element_ids_for_topo(const conduit::Node &n_topo,
                                          const index_t erange[2],
                                          std::vector<index_t> &element_ids) const = 0;

protected:
    static const std::string DOMAIN_KEY;
    static const std::string MAPPING_KEY;

    const conduit::Node *n_options_ptr;
};

const std::string selection::DOMAIN_KEY("domain");
const std::string selection::MAPPING_KEY("mapping");


//---------------------------------------------------------------------------
/**
 @brief This class represents a logical IJK selection with start and end
        values. Start begins at 0 and End is the size of the mesh (in terms
        of cells) minus 1.

        A cell with 10x10x10 cells would have the following selection to
        select it all: start={0,0,0}, end={9,9,9}. To select a single cell,
        make start the same as end.
*/
class selection_logical : public selection
{
public:
    selection_logical();
    virtual ~selection_logical();

    // Initializes the selection from a conduit::Node.
    virtual bool init(const conduit::Node *n_opt_ptr) override;

    virtual bool applicable(const conduit::Node &n_mesh) override;

    // Computes the number of cells in the selection.
    virtual index_t length() const override
    {
        return cells_for_axis(0) * 
               cells_for_axis(1) * 
               cells_for_axis(2);
    }

    virtual std::vector<std::shared_ptr<selection> > partition(const conduit::Node &n_mesh) const override;

    void set_start(index_t s0, index_t s1, index_t s2)
    {
        start[0] = s0;
        start[1] = s1;
        start[2] = s2;
    }

    void set_end(index_t e0, index_t e1, index_t e2)
    {
        end[0] = e0;
        end[1] = e1;
        end[2] = e2;
    }

    virtual void get_element_ids_for_topo(const conduit::Node &n_topo,
                                          const index_t erange[2],
                                          std::vector<index_t> &element_ids) const override;
private:
    index_t cells_for_axis(int axis) const
    {
        index_t delta = static_cast<index_t>(end[axis] - start[axis] + 1);
        constexpr index_t one = 1;
        return (axis >= 0 && axis <= 2) ? std::max(delta, one) : 0;
    }

    index_t start[3];
    index_t end[3];
};

//---------------------------------------------------------------------------
selection_logical::selection_logical() : selection()
{
    start[0] = start[1] = start[2] = 0;
    end[0] = end[1] = end[2] = 0;
}

//---------------------------------------------------------------------------
selection_logical::~selection_logical()
{
}

//---------------------------------------------------------------------------
bool
selection_logical::init(const conduit::Node *n_opt_ptr)
{
    bool ok = false;
    n_options_ptr = n_opt_ptr;
    if(n_options_ptr->has_child("start") && n_options_ptr->has_child("end"))
    {
        unsigned_int_array s = n_options_ptr->child("start").value();
        unsigned_int_array e = n_options_ptr->child("end").value();
        if(s.number_of_elements() == 3 &&
           e.number_of_elements() == 3)
        {
            for(int i = 0; i < 3; i++)
            {
                start[i] = static_cast<index_t>(s[i]);
                end[i] = static_cast<index_t>(e[i]);
            }
            ok = true;
        }
    }
    return ok;
}

//---------------------------------------------------------------------------
/**
 @brief Returns whether the logical selection applies to the input mesh.
 */
bool
selection_logical::applicable(const conduit::Node &n_mesh)
{
    bool retval = false;

    const conduit::Node &n_coords = n_mesh["coordsets"][0];
    const conduit::Node &n_topo = n_mesh["topologies"][0];
    bool is_uniform = n_coords["type"].as_string() == "uniform";
    bool is_rectilinear = n_coords["type"].as_string() == "rectilinear";
    bool is_structured = n_coords["type"].as_string() == "explicit" && 
                         n_topo["type"].as_string() == "structured";
    if(is_uniform || is_rectilinear || is_structured)
    {
        index_t dims[3] = {1,1,1};
        const conduit::Node &n_topo = n_mesh["topologies"][0];
        conduit::blueprint::mesh::utils::topology::logical_dims(n_topo, dims, 3);

        // See that the selection starts inside the dimensions.
        if(start[0] < dims[0] && start[1] < dims[1] && start[2] < dims[2])
        {
            // Clamp the selection to the dimensions of the mesh.
            end[0] = std::min(end[0], dims[0]-1);
            end[1] = std::min(end[1], dims[1]-1);
            end[2] = std::min(end[2], dims[2]-1);

            retval = true;
        }
    }

    return retval;
}

//---------------------------------------------------------------------------
/**
 @brief Partitions along the longest axis and returns a vector containing 2
        logical selections.
 */
std::vector<std::shared_ptr<selection> >
selection_logical::partition(const conduit::Node &/*n_mesh*/) const
{
    int la = 0;
    if(cells_for_axis(1) > cells_for_axis(la))
        la = 1;
    if(cells_for_axis(2) > cells_for_axis(la))
        la = 2;
    auto n = cells_for_axis(la);

    auto p0 = std::shared_ptr<selection_logical>();
    auto p1 = std::shared_ptr<selection_logical>();
    if(la == 0)
    {
        p0->set_start(start[0],       start[1],       start[2]);
        p0->set_end(start[0]+n/2,     end[1],         end[2]);
        p1->set_start(start[0]+n/2+1, start[1],       start[2]);
        p1->set_end(end[0],           end[1],         end[2]);
    }
    else if(la == 1)
    {
        p0->set_start(start[0],       start[1],       start[2]);
        p0->set_end(start[0],         end[1]+n/2,     end[2]);
        p1->set_start(start[0],       start[1]+n/2+1, start[2]);
        p1->set_end(end[0],           end[1],         end[2]);
    }
    else
    {
        p0->set_start(start[0],       start[1],       start[2]);
        p0->set_end(start[0],         end[1],         end[2]+n/2);
        p1->set_start(start[0],       start[1],       start[2]+n/2+1);
        p1->set_end(end[0],           end[1],         end[2]);
    }

    std::vector<std::shared_ptr<selection> > parts;
    parts.push_back(p0);
    parts.push_back(p1);

    return parts;
}

//---------------------------------------------------------------------------
/*void
selection_logical::get_vertex_ids(const conduit::Node &n_mesh,
    std::vector<index_t> &ids) const
{
    index_t dims[3] = {1,1,1};
    const conduit::Node &n_topo = n_mesh["topologies"][0];
    topology::logical_dims(n_topo, dims, 3);

    ids.clear();
    ids.reserve(dims[0] * dims[1] * dims[2]);
    auto mesh_NXNY = dims[0] * dims[1];
    auto mesh_NX   = dims[0];  
    index_t n_end[3];
    n_end[0] = end[0] + 1;
    n_end[1] = end[1] + 1;
    n_end[2] = end[2] + 1;
    for(index_t k = start[2]; k <= n_end[2]; k++)
    for(index_t j = start[1]; j <= n_end[1]; j++)
    for(index_t i = start[0]; i <= n_end[0]; i++)
    {
        ids.push_back(k*mesh_NXNY + j*mesh_NX + i);
    }
}
*/

//---------------------------------------------------------------------------
void
selection_logical::get_element_ids_for_topo(const conduit::Node &n_topo,
    const index_t erange[2], std::vector<index_t> &element_ids) const
{
    index_t dims[3] = {1,1,1};
    conduit::blueprint::mesh::utils::topology::logical_dims(n_topo, dims, 3);

    element_ids.clear();
    element_ids.reserve(length());
    auto mesh_CXCY = dims[0] * dims[1];
    auto mesh_CX   = dims[0];
    for(index_t k = start[2]; k <= end[2]; k++)
    for(index_t j = start[1]; j <= end[1]; j++)
    for(index_t i = start[0]; i <= end[0]; i++)
    {
        auto eid = k*mesh_CXCY + j*mesh_CX + i;
        if(eid >= erange[0] && eid <= erange[1])
            element_ids.push_back(eid);
    }
}

//---------------------------------------------------------------------------
/**
   @brief This selection explicitly defines which cells we're pulling out from
          a mesh, and in which order.
 */
class selection_explicit : public selection
{
public:
    selection_explicit() : selection(), ids_storage(),
        num_cells_in_selection(0), num_cells_in_mesh(0)
    {
    }

    virtual ~selection_explicit()
    {
    }

    virtual bool init(const conduit::Node *n_opt_ptr) override
    {
        bool ok = false;
        n_options_ptr = n_opt_ptr;
        if(n_options_ptr &&
           n_options_ptr->has_child(ELEMENTS_KEY) &&
           n_options_ptr->child(ELEMENTS_KEY).dtype().is_number())
        {
            // Convert to the right type for index_t
#ifdef CONDUIT_INDEX_32
            n_options_ptr->child(ELEMENTS_KEY).to_uint32_array(ids_storage);
#else
            n_options_ptr->child(ELEMENTS_KEY).to_uint64_array(ids_storage);
#endif
            ok = true;
        }
        return ok;
    }

    virtual bool applicable(const conduit::Node &n_mesh) override;

    // Computes the number of cells in the selection.
    virtual index_t length() const override
    {
        return num_cells_in_selection;
    }

    virtual std::vector<std::shared_ptr<selection> > partition(const conduit::Node &n_mesh) const override;

    const index_t *get_indices() const
    {
        // Access the converted data as index_t.
        return reinterpret_cast<const index_t *>(ids_storage.data_ptr());
    }

    virtual void get_element_ids_for_topo(const conduit::Node &n_topo,
                                          const index_t erange[2],
                                          std::vector<index_t> &element_ids) const override;

private:
    static const std::string ELEMENTS_KEY;
    conduit::Node ids_storage;
    index_t num_cells_in_selection;
    index_t num_cells_in_mesh;
};

const std::string selection_explicit::ELEMENTS_KEY("elements");

//---------------------------------------------------------------------------
/**
 @brief Returns whether the explicit selection applies to the input mesh.
 */
bool
selection_explicit::applicable(const conduit::Node &/*n_mesh*/)
{
    return true;
}

//---------------------------------------------------------------------------
std::vector<std::shared_ptr<selection> >
selection_explicit::partition(const conduit::Node &n_mesh) const
{
    auto num_cells_in_mesh = topology::length(n_mesh);
    auto n = ids_storage.dtype().number_of_elements();
    auto n_2 = n/2;
    auto indices = get_indices();
    std::vector<index_t> ids0, ids1;
    ids0.reserve(n_2);
    ids1.reserve(n_2);
    for(index_t i = 0; i < n; i++)
    {
        if(indices[i] < num_cells_in_mesh)
        {
            if(i < n_2)
                ids0.push_back(indices[i]);
            else
                ids1.push_back(indices[i]);
        }
    }

    auto p0 = std::make_shared<selection_explicit>();
    auto p1 = std::make_shared<selection_explicit>();
    p0->ids_storage.set(ids0);
    p0->num_cells_in_selection = ids0.size(); 
    p0->num_cells_in_mesh = num_cells_in_mesh;

    p1->ids_storage.set(ids1);
    p1->num_cells_in_selection = ids1.size(); 
    p1->num_cells_in_mesh = num_cells_in_mesh;

    std::vector<std::shared_ptr<selection> > parts;
    parts.push_back(p0);
    parts.push_back(p1);

    return parts;
}

//---------------------------------------------------------------------------
void
selection_explicit::get_element_ids_for_topo(const conduit::Node &/*n_topo*/,
    const index_t erange[2], std::vector<index_t> &element_ids) const
{
    auto n = ids_storage.dtype().number_of_elements();
    auto indices = get_indices();
    element_ids.reserve(n);
    for(index_t i = 0; i < n; i++)
    {
        auto eid = indices[i];
        if(eid >= erange[0] && eid <= erange[1])
            element_ids.push_back(eid);
    }
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
class selection_ranges : public selection
{
public:
    selection_ranges() : selection(), ranges_storage()
    {
    }

    virtual ~selection_ranges()
    {
    }

    virtual bool init(const conduit::Node *n_opt_ptr) override
    {
        bool ok = false;
        n_options_ptr = n_opt_ptr;
        if(n_options_ptr &&
           n_options_ptr->has_child(RANGES_KEY) &&
           n_options_ptr->child(RANGES_KEY).dtype().is_number())
        {
            // Convert to the right type for index_t
#ifdef CONDUIT_INDEX_32
            n_options_ptr->child(RANGES_KEY).to_uint32_array(ranges_storage);
#else
            n_options_ptr->child(RANGES_KEY).to_uint64_array(ranges_storage);
#endif
            ok = (ranges_storage.dtype().number_of_elements() % 2 == 0);
        }
        return ok;
    }

    virtual bool applicable(const conduit::Node &n_mesh) override;

    // Computes the number of cells in the selection.
    virtual index_t length() const override;

    virtual std::vector<std::shared_ptr<selection> > partition(const conduit::Node &n_mesh) const override;

    virtual void get_element_ids_for_topo(const conduit::Node &n_topo,
                                          const index_t erange[2],
                                          std::vector<index_t> &element_ids) const override;

    const index_t *get_ranges() const
    {
        // Access the converted data as index_t.
        return reinterpret_cast<const index_t *>(ranges_storage.data_ptr());
    }

private:
    static const std::string RANGES_KEY;
    conduit::Node ranges_storage;
};

const std::string selection_ranges::RANGES_KEY("ranges");

//---------------------------------------------------------------------------
bool
selection_ranges::applicable(const conduit::Node &/*n_mesh*/)
{
    return true;
}

//---------------------------------------------------------------------------
index_t
selection_ranges::length() const
{
    index_t ncells = 0;
    const index_t *ranges = get_ranges();
    auto n = ranges_storage.dtype().number_of_elements() / 2;
    for(index_t i = 0; i < n; i++)
    {
        ncells += ranges[2*i+1] - ranges[2*i] + 1;
    }
    return ncells;
}

//---------------------------------------------------------------------------
std::vector<std::shared_ptr<selection> >
selection_ranges::partition(const conduit::Node &/*n_mesh*/) const
{
    index_t ncells = length();
    auto ncells_2 = ncells / 2;
    auto n = ranges_storage.dtype().number_of_elements() / 2;
    auto ranges = get_ranges();
    index_t count = 0;
    index_t split_index = 0;
    for(index_t i = 0; i < n; i++)
    {
        auto rc = ranges[2*i+1] - ranges[2*i] + 1;
        if(count + rc > ncells_2)
        {
            split_index = i;
            break;
        }
        else
        {
            count += rc;
        }
    }

    std::vector<index_t> r0, r1;
    for(index_t i = 0; i < n; i++)
    {
        if(i < split_index)
        {
            r0.push_back(ranges[2*i+0]);
            r0.push_back(ranges[2*i+1]);
        }
        else if(i == split_index)
        {
            auto rc = (ranges[2*i+1] - ranges[2*i]) + 1;
            if(rc == 1)
            {
                r0.push_back(ranges[2*i+0]);
                r0.push_back(ranges[2*i+0]);
            }
            else if(rc == 2)
            {
                r0.push_back(ranges[2*i+0]);
                r0.push_back(ranges[2*i+0]);

                r1.push_back(ranges[2*i+1]);
                r1.push_back(ranges[2*i+1]);
            }
            else
            {
                auto rc_2 = rc / 2;
                r0.push_back(ranges[2*i+0]);
                r0.push_back(ranges[2*i+0] + rc_2);

                r1.push_back(ranges[2*i+0] + rc_2 + 1);
                r1.push_back(ranges[2*i+1]);
            }
        }
        else //if(i > split_index)
        {
            r1.push_back(ranges[2*i+0]);
            r1.push_back(ranges[2*i+1]);
        }
    }

    auto p0 = std::make_shared<selection_ranges>();
    auto p1 = std::make_shared<selection_ranges>();
    p0->ranges_storage.set(r0);
    p1->ranges_storage.set(r1);

    std::vector<std::shared_ptr<selection> > parts;
    parts.push_back(p0);
    parts.push_back(p1);

    return parts;
}

//---------------------------------------------------------------------------
void
selection_ranges::get_element_ids_for_topo(const conduit::Node &/*n_topo*/,
    const index_t erange[2], std::vector<index_t> &element_ids) const
{
    auto n = ranges_storage.dtype().number_of_elements();
    auto n_2 = n / 2;
    auto indices = get_ranges();
    for(index_t i = 0; i < n_2; i++)
    {
        index_t start = indices[2*i];
        index_t end = indices[2*i+1];
        for(index_t eid = start; eid <= end; eid++)
        {
            if(eid >= erange[0] && eid <= erange[1])
                element_ids.push_back(eid);
        }
    }
}


//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
/**
 @brief This class can read a set of selections and apply them to a Conduit
        node containing single or multi-domain meshes and produce a new
        Conduit node that refashions the selections into a target number of
        mesh domains.
 */
class partitioner
{
public:
    /**
     @brief This struct is a Conduit/Blueprint mesh plus an ownership bool.
            The mesh pointer is always assumed to be external by default
            so we do not provide a destructor. If we want to delete it,
            call free(), which will free the mesh if we own it.
     */
    struct chunk
    {
        chunk();
        chunk(const Node *m, bool own);
        void free();

        const Node *mesh;
        bool        owns;
    };

    /**
     @brief Constructor.
     */
    partitioner();

    /**
     @brief Destructor.
     */
    virtual ~partitioner();

    /**
     @brief Initialize the partitioner using the input mesh (which could be
            multidomain) and a set of options. The options specify how we
            may be pulling apart the mesh using selections. The selections
            are allowed to be empty, in which case we're using all of the
            input mesh domains.

     @note We initialize before execute() because we may want the opportunity
           to split selections into an appropriate target number of domains.

     @param n_mesh A Conduit node representing the Blueprint mesh.
     @param options A Conduit node containing the partitioning options.

     @return True if the options were accepted or False if they contained
             an error.
     */
    bool initialize(const conduit::Node &n_mesh, const conduit::Node &options);

    /**
     @brief This method enables the partitioner to split the selections until
            we arrive at a number of selections that will yield the desired
            number of target domains.

     @note We could oversplit at some point if we're already unstructured
           if we want to load balance mesh sizes a little better.
     */
    virtual void split_selections();

    /**
     @brief Execute the partitioner so we use any options to arrive at the
            target number of domains. This may involve splitting domains,
            redistributing them (in parallel), and then combining them to
            populate the output node.

     @param[out] output The Conduit node that will receive the output meshes.
     */
    void execute(conduit::Node &output);


    /**
     @brief Given a vector of input Blueprint meshes, we combine them into 
            a single Blueprint mesh stored in output.

     @param domain The domain number being created out of the various inputs.
     @param inputs A vector of Blueprint meshes representing the input
                   meshes that will be combined.
     @param[out] output The Conduit node to which the combined mesh's properties
                        will be added.

     @note This method is exposed so we can create a partitioner object and 
           combine meshes with it directly, which would be useful for development
           and unit tests. This method is serial only and will only operate on
           its inputs to generate the single combined mesh for the output node.
     */
    void combine(int domain,
                 const std::vector<const Node *> &inputs,
                 Node &output);

protected:
    /**
     @brief Compute the total number of selections across all ranks.
     @return The total number of selections across all ranks.

     @note Reimplement in parallel
     */
    virtual long get_total_selections() const;

    /**
     @brief Get the rank and index of the largest selection. In parallel, we
            will be looking across all ranks to find the largest domains so
            those are split first.

     @note  Splitting one at a time is temporary since in parallel, it's not
            good enough.

     @param[out] sel_rank The rank that contains the largest selection.
     @param[out] sel_index The index of the largest selection on sel_rank.

     @note Reimplement in parallel
     */
    virtual void get_largest_selection(int &sel_rank, int &sel_index) const;

    /**
     @brief This is a factory method for creating selections based on the 
            provided options.
     @param n_sel A Conduit node that represents the options used for
                  creating the selection.
     @return A new instance of a selection that best represents the options.
     */
    std::shared_ptr<selection> create_selection(const conduit::Node &n_sel) const;

    void copy_fields(const std::vector<index_t> &all_selected_vertex_ids,
                     const std::vector<index_t> &all_selected_element_ids,
                     const conduit::Node &n_mesh,
                     conduit::Node &output,
                     bool preserve_mapping) const;

    void copy_field(const conduit::Node &n_field,
                    const std::vector<index_t> &ids,
                    Node &n_output_fields) const;

    void slice_array(const conduit::Node &n_src_values,
                     const std::vector<index_t> &ids,
                     Node &n_dest_values) const;

    void get_vertex_ids_for_element_ids(const conduit::Node &n_topo,
             const std::vector<index_t> &element_ids,
             std::set<index_t> &vertex_ids) const;

    /**
     @brief Extract the idx'th selection from the input mesh and return a
            new Node containing the extracted chunk.

     @param idx The selection to extract. This must be a valid selection index.
     @param n_mesh A Conduit node representing the mesh to which we're
                   applying the selection.
     @return A new Conduit node (to be freed by caller) that contains the
             extracted chunk.
     */
    conduit::Node *extract(size_t idx, const conduit::Node &n_mesh) const;

    void create_new_explicit_coordset(const conduit::Node &n_coordset,
             const std::vector<index_t> &vertex_ids,
             conduit::Node &n_new_coordset) const;

    void create_new_unstructured_topo(const conduit::Node &n_topo,
             const std::vector<index_t> &element_ids,
             const std::vector<index_t> &vertex_ids,
             conduit::Node &n_new_topo) const;

    void unstructured_topo_from_unstructured(const conduit::Node &n_topo,
             const std::vector<index_t> &element_ids,
             const std::vector<index_t> &vertex_ids,
             conduit::Node &n_new_topo) const;

    /**
     @brief Given a local set of chunks, figure out starting domain index
            that will be used when numbering domains on a rank. We figure
            out which ranks get domains and this is the scan of the domain
            numbers.
     
     @return The starting domain index on this rank.
     */
    virtual unsigned int starting_index(const std::vector<chunk> &chunks);

    /**
     @brief Assign the chunks on this rank a destination rank to which it will
            be transported as well as a destination domain that indices which
            chunks will be combined into the final domains.

     @note All chunks that get the same dest_domain must also get the same
           dest_rank since dest_rank is the MPI rank that will do the work
           of combining the chunks it gets. Also, this method nominates
           ranks as those who will receive chunks. If there are 4 target
           chunks then when we run in parallel, 4 ranks will get a domain.
           This will be overridden for parallel.

     @param chunks A vector of input chunks that we are mapping to ranks
                   and domains.
     @param[out] dest_ranks The destination ranks that get each input chunk.
     @param[out] dest_domain The destination domain to which a chunk is assigned.
     */
    virtual void map_chunks(const std::vector<chunk> &chunks,
                            std::vector<int> &dest_ranks,
                            std::vector<int> &dest_domain);

    /**
     @brief Communicates the input chunks to their respective destination ranks
            and passes out the set of chunks that this rank will operate on in
            the chunks_to_assemble vector.

     @param chunks The vector of input chunks that may be redistributed to
                   different ranks.
     @param dest_rank A vector of integers containing the destination ranks of
                      each chunk.
     @param dest_domain The global numbering of each input chunk.
     @param[out] chunks_to_assemble The vector of chunks that this rank will
                                    combine and set into the output.
     @param[out] chunks_to_assemble_domains The global domain numbering of each
                                            chunk in chunks_to_assemble. Like-
                                            numbered chunks will be combined
                                            into a single output domain.

     @note This will be overridden for parallel.
     */
    virtual void communicate_chunks(const std::vector<chunk> &chunks,
                                    const std::vector<int> &dest_rank,
                                    const std::vector<int> &dest_domain,
                                    std::vector<chunk> &chunks_to_assemble,
                                    std::vector<int> &chunks_to_assemble_domains);

    int rank, size;
    unsigned int target;
    std::vector<const Node *>                meshes;
    std::vector<std::shared_ptr<selection> > selections;
};

//---------------------------------------------------------------------------
partitioner::chunk::chunk() : mesh(nullptr), owns(false)
{
}

//---------------------------------------------------------------------------
partitioner::chunk::chunk(const Node *m, bool own) : mesh(m), owns(own)
{
}

//---------------------------------------------------------------------------
void
partitioner::chunk::free()
{
    if(owns)
    {
        Node *m = const_cast<Node *>(mesh);
        delete m;
        mesh = nullptr;
        owns = false;
    }
}

//---------------------------------------------------------------------------
partitioner::partitioner() : rank(0), size(1), target(1), meshes(), selections()
{
}

//---------------------------------------------------------------------------
partitioner::~partitioner()
{
}

//---------------------------------------------------------------------------
std::shared_ptr<selection>
partitioner::create_selection(const conduit::Node &n_sel) const
{
    std::shared_ptr<selection> retval;
    if(n_sel["type"].as_string() == "logical")
        retval = std::make_shared<selection_logical>();
    else if(n_sel["type"].as_string() == "explicit")
        retval = std::make_shared<selection_explicit>();
    else if(n_sel["type"].as_string() == "ranges")
        retval = std::make_shared<selection_ranges>();
    return retval;
}

//---------------------------------------------------------------------------
bool
partitioner::initialize(const conduit::Node &n_mesh, const conduit::Node &options)
{
    auto doms = conduit::blueprint::mesh::domains(n_mesh);

    // Iterate over the selections in the options and check them against the
    // domains that were passed in to make a vector of meshes and selections
    // that can be used to partition the meshes.
    if(options.has_child("selections"))
    {
        const conduit::Node &n_selections = options["selections"];
        for(index_t i = 0; i < n_selections.number_of_children(); i++)
        {
            const conduit::Node *n_sel = n_selections.child_ptr(i);
            auto sel = create_selection(*n_sel);
            if(sel != nullptr && sel->init(n_sel))
            {
                // The selection is good. See if it applies to the domains.
                auto n = static_cast<index_t>(doms.size());
                for(index_t domid = 0; n; domid++)
                {
                    // Q: What is the domain number for this domain?

                    if(domid == sel->get_domain() && sel->applicable(*doms[domid]))
                    {
                        meshes.push_back(doms[domid]);
                        selections.push_back(sel);
                        break;
                    }
                }
            }
        }
    }
    else
    {
        // Add null selections to indicate that we take the whole domain.
        for(size_t domid = 0; domid < doms.size(); domid++)
        {
            meshes.push_back(doms[domid]);
            selections.push_back(nullptr);
        }
    }

    // Get the number of target partitions that we're making.
    target = 1;
    if(options.has_child("target"))
        target = options.as_unsigned_int();

    return !selections.empty();
}

//---------------------------------------------------------------------------
void
partitioner::get_largest_selection(int &sel_rank, int &sel_index) const
{
    sel_rank = 0;
    long largest_selection_size = 0;
    for(size_t i = 0; i < selections.size(); i++)
    {
        long ssize = static_cast<long>(selections[i]->length());
        if(ssize > largest_selection_size)
        {
            largest_selection_size = ssize;
            sel_index = static_cast<int>(i);
        }
    }
}

//---------------------------------------------------------------------------
long
partitioner::get_total_selections() const
{
    return static_cast<long>(selections.size());
}

//---------------------------------------------------------------------------
void
partitioner::split_selections()
{
    long ntotal_selections = get_total_selections();

    // Splitting.
    while(target > ntotal_selections)
    {
        // Get the rank with the largest selection and get that local
        // selection index.
        int sel_rank = -1, sel_index = -1;
        get_largest_selection(sel_rank, sel_index);

        if(rank == sel_rank)
        {
            auto ps = selections[sel_index]->partition(*meshes[sel_index]);

            if(!ps.empty())
            {
                const conduit::Node *m = meshes[sel_index];
                meshes.insert(meshes.begin()+sel_index, ps.size()-1, m);
                selections.insert(selections.begin()+sel_index, ps.size()-1, nullptr);
                for(int i = 0; i < sel_index; i++)
                    selections[sel_index + i] = ps[i];
            }
        }
    }
}

//---------------------------------------------------------------------------
void
partitioner::copy_fields(const std::vector<index_t> &all_selected_vertex_ids,
    const std::vector<index_t> &all_selected_element_ids,
    const conduit::Node &n_mesh,
    conduit::Node &n_output,
    bool preserve_mapping) const
{
    if(n_mesh.has_child("fields"))
    {
        const conduit::Node &n_fields = n_mesh["fields"];
        if(!all_selected_vertex_ids.empty())
        {
            conduit::Node &n_output_fields = n_output["fields"];
            for(index_t i = 0; i < n_fields.number_of_children(); i++)
            {
                const conduit::Node &n_field = n_fields[i];
                if(n_field.has_child("association"))
                {
                    auto association = n_field["association"].as_string();
                    if(association == "vertex")
                    {
                        copy_field(n_field, all_selected_vertex_ids, n_output_fields);
                    }
                }
            }

            if(preserve_mapping)
            {
                // TODO: save the all_selected_vertex_ids as a new field.
            }
        }

        if(!all_selected_element_ids.empty())
        {
            conduit::Node &n_output_fields = n_output["fields"];
            for(index_t i = 0; i < n_fields.number_of_children(); i++)
            {
                const conduit::Node &n_field = n_fields[i];
                if(n_field.has_child("association"))
                {
                    auto association = n_field["association"].as_string();
                    if(association == "element")
                    {
                        copy_field(n_field, all_selected_element_ids, n_output_fields);
                    }
                }
            }

            if(preserve_mapping)
            {
                // TODO: save the all_selected_element_ids as a new field.
            }
        }
    }
}

//---------------------------------------------------------------------------
void
partitioner::copy_field(const conduit::Node &n_field,
    const std::vector<index_t> &ids, Node &n_output_fields) const
{
    static const std::vector<std::string> keys{"association", "grid_function",
        "volume_dependent", "topology"};

// TODO: What about matsets and mixed fields?...
// https://llnl-conduit.readthedocs.io/en/latest/blueprint_mesh.html#fields

    // Copy common field attributes from the old field into the new one.
    conduit::Node &n_new_field = n_output_fields[n_field.name()];
    for(const auto &key : keys)
    {
        if(n_field.has_child(key))
            n_new_field[key] = n_field[key];
    }

    const conduit::Node &n_values = n_field["values"];
    if(n_values.dtype().is_compact()) 
    {
        slice_array(n_values, ids, n_new_field["values"]);
    }
    else
    {
        // otherwise, we need to compact our data first
        conduit::Node n;
        n_values.compact_to(n);
        slice_array(n, ids, n_new_field["values"]);
    }
}

//---------------------------------------------------------------------------
// @brief Slice the n_src array using the indices stored in ids. The 
//        destination memory is already pointed to by n_dest.
template <typename T>
inline void
typed_slice_array(const T *src, const std::vector<index_t> &ids, T *dest)
{
    size_t n = ids.size();
    for(size_t i = 0; i < n; i++)
        dest[i] = src[ids[i]];
}

//---------------------------------------------------------------------------
// @note Should this be part of conduit::Node or DataArray somehow. The number
//       of times I've had to slice an array...
void
partitioner::slice_array(const conduit::Node &n_src_values,
    const std::vector<index_t> &ids, Node &n_dest_values) const
{
    // Copy the DataType of the input conduit::Node but override the number of elements
    // before copying it in so assigning to n_dest_values triggers a memory
    // allocation.
    auto dt = n_src_values.dtype();
    n_dest_values = DataType(n_src_values.dtype().id(), ids.size());

    // Do the slice.
    if(dt.is_int8())
        typed_slice_array(reinterpret_cast<const conduit::int8 *>(n_src_values.data_ptr()), ids,
                    reinterpret_cast<conduit::int8 *>(n_dest_values.data_ptr()));
    else if(dt.is_int16())
        typed_slice_array(reinterpret_cast<const int16 *>(n_src_values.data_ptr()), ids,
                    reinterpret_cast<conduit::int16 *>(n_dest_values.data_ptr()));
    else if(dt.is_int32())
        typed_slice_array(reinterpret_cast<const conduit::int32 *>(n_src_values.data_ptr()), ids,
                    reinterpret_cast<conduit::int32 *>(n_dest_values.data_ptr()));
    else if(dt.is_int64())
        typed_slice_array(reinterpret_cast<const conduit::int64 *>(n_src_values.data_ptr()), ids,
                    reinterpret_cast<conduit::int64 *>(n_dest_values.data_ptr()));
    else if(dt.is_uint8())
        typed_slice_array(reinterpret_cast<const conduit::uint8 *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<conduit::uint8 *>(n_dest_values.data_ptr()));
    else if(dt.is_uint16())
        typed_slice_array(reinterpret_cast<const uint16 *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<conduit::uint16 *>(n_dest_values.data_ptr()));
    else if(dt.is_uint32())
        typed_slice_array(reinterpret_cast<const conduit::uint32 *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<conduit::uint32 *>(n_dest_values.data_ptr()));
    else if(dt.is_uint64())
        typed_slice_array(reinterpret_cast<const conduit::uint64 *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<conduit::uint64 *>(n_dest_values.data_ptr()));
    else if(dt.is_char())
        typed_slice_array(reinterpret_cast<const char *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<char *>(n_dest_values.data_ptr()));
    else if(dt.is_short())
        typed_slice_array(reinterpret_cast<const short *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<short *>(n_dest_values.data_ptr()));
    else if(dt.is_int())
        typed_slice_array(reinterpret_cast<const int *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<int *>(n_dest_values.data_ptr()));
    else if(dt.is_long())
        typed_slice_array(reinterpret_cast<const long *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<long *>(n_dest_values.data_ptr()));
    else if(dt.is_unsigned_char())
        typed_slice_array(reinterpret_cast<const unsigned char *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<unsigned char *>(n_dest_values.data_ptr()));
    else if(dt.is_unsigned_short())
        typed_slice_array(reinterpret_cast<const unsigned short *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<unsigned short *>(n_dest_values.data_ptr()));
    else if(dt.is_unsigned_int())
        typed_slice_array(reinterpret_cast<const unsigned int *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<unsigned int *>(n_dest_values.data_ptr()));
    else if(dt.is_unsigned_long())
        typed_slice_array(reinterpret_cast<const unsigned long *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<unsigned long *>(n_dest_values.data_ptr()));
    else if(dt.is_float())
        typed_slice_array(reinterpret_cast<const float *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<float *>(n_dest_values.data_ptr()));
    else if(dt.is_double())
        typed_slice_array(reinterpret_cast<const double *>(n_src_values.data_ptr()), ids,
                          reinterpret_cast<double *>(n_dest_values.data_ptr()));
}

//---------------------------------------------------------------------------
/**
 @brief Iterates over the cells in the topo that are specified in element_ids
        and adds their vertex ids into vertex_ids so we can build up a set of
        vertices that will need to be pulled from the coordset.
 */
void
partitioner::get_vertex_ids_for_element_ids(const conduit::Node &n_topo,
    const std::vector<index_t> &element_ids,
    std::set<index_t> &vertex_ids) const
{
    bool is_base_rectilinear = n_topo["type"].as_string() == "rectilinear";
    bool is_base_structured = n_topo["type"].as_string() == "structured";
    bool is_base_uniform = n_topo["type"].as_string() == "uniform";

    if(is_base_rectilinear || is_base_structured || is_base_uniform)
    {
        index_t edims[3] = {1,1,1}, dims[3] = {0,0,0};
        auto ndims = topology::dims(n_topo);
        conduit::blueprint::mesh::utils::topology::logical_dims(n_topo, edims, 3);
        dims[0] = edims[0] + 1;
        dims[1] = edims[1] + 1;
        dims[2] = edims[2] + 1;

        index_t cell_ijk[3]={0,0,0}, pt_ijk[3] = {0,0,0}, ptid = 0;
        static const index_t offsets[8][3] = {
            {0,0,0},
            {1,0,0},
            {0,1,0},
            {1,1,0},
            {0,0,1},
            {1,0,1},
            {0,1,1},
            {1,1,1}
        };
        int np = (ndims == 2) ? 4 : 8;
        auto n = element_ids.size();
        for(size_t i = 0; i < n; i++)
        {
            // Get the IJK coordinate of the element.
            grid_id_to_ijk(element_ids[i], edims, cell_ijk);

            // Turn the IJK into vertex ids.
            for(int i = 0; i < np; i++)
            {
                pt_ijk[0] = cell_ijk[0] + offsets[i][0];
                pt_ijk[1] = cell_ijk[1] + offsets[i][1];
                pt_ijk[2] = cell_ijk[2] + offsets[i][2];
                grid_ijk_to_id(pt_ijk, dims, ptid);

                vertex_ids.insert(ptid);
            }
        }
    }
    else
    {
        const conduit::Node &n_conn = n_topo["elements/connectivity"];
// Conduit needs to_index_t_array() and as_index_t_ptr() methods.
        conduit::Node indices;
#ifdef CONDUIT_INDEX_32
        n_conn.to_unsigned_int_array(indices);
        auto iptr = indices.as_unsigned_int_ptr();
#else
        n_conn.to_unsigned_long_array(indices);
        auto iptr = indices.as_unsigned_long_ptr();
#endif
        conduit::blueprint::mesh::utils::ShapeType shape(n_topo);
        if(shape.is_poly())
        {
            // TODO:
        }
        else if(shape.is_polygonal())
        {
            // TODO:
        }
        else if(shape.is_polyhedral())
        {
            // TODO:
        }
        else
        {
            // Shapes are single types one after the next in the connectivity.
            auto nverts_in_shape = conduit::blueprint::mesh::utils::TOPO_SHAPE_INDEX_COUNTS[shape.id];
            for(size_t i = 0; i < element_ids.size(); i++)
            {
                auto elem_conn = iptr + element_ids[i] * nverts_in_shape;
                for(index_t j = 0; j < nverts_in_shape; j++)
                    vertex_ids.insert(elem_conn[j]);
            }
        }
    }
}

//---------------------------------------------------------------------------
inline void
index_t_set_to_vector(const std::set<index_t> &src, std::vector<index_t> &dest)
{
    dest.reserve(src.size());
    for(auto it = src.begin(); it != src.end(); it++)
        dest.push_back(*it);
}

//---------------------------------------------------------------------------
conduit::Node *
partitioner::extract(size_t idx, const conduit::Node &n_mesh) const
{
    if(idx >= selections.size())
        return nullptr;

    const conduit::Node &n_topo = n_mesh["topologies"];
    const conduit::Node &n_coordsets = n_mesh["coordsets"];
    std::map<std::string, std::shared_ptr<std::vector<index_t>>> topo_element_ids;
    std::map<std::string, std::shared_ptr<std::set<index_t>>> coordset_vertex_ids;
    index_t erange[] = {0,0};
    for(index_t i = 0; i < n_topo.number_of_children(); i++)
    {
        // Get the current topology.
        const conduit::Node &this_topo = n_topo[i];

        // Get the number of elements in the topology.
        index_t topo_num_elements = topology::length(this_topo);
        erange[1] += topo_num_elements-1;

        // Create a vector to which we'll add the element ids for this topo.
        // We have to keep it around 
        auto eit = topo_element_ids.find(this_topo.name());
        if(eit == topo_element_ids.end())
        {
            topo_element_ids[this_topo.name()] = std::make_shared<std::vector<index_t>>();
            eit = topo_element_ids.find(this_topo.name());
        }
        // Get the selected element ids that are in this topology.
        std::vector<index_t> &element_ids = *eit->second;

// NOTE: we could pass back ranges for the element ids...
        // Get the selected element ids that are in this topology.
        selections[idx]->get_element_ids_for_topo(this_topo, erange, element_ids);

        // What's its coordset name?
        std::string csname(this_topo["coordset"].name());
        auto vit = coordset_vertex_ids.find(csname);
        if(vit == coordset_vertex_ids.end())
        {
            coordset_vertex_ids[csname] = std::make_shared<std::set<index_t>>();
            vit = coordset_vertex_ids.find(csname);
        }

// NOTE: we could pass back ranges for the vertex ids...
        // Add the vertex ids for the elements in the selection. This lets us
        // build up a comprehensive set of the vertex ids we need from the
        // coordset, as determined by multiple topologies.
        get_vertex_ids_for_element_ids(this_topo, *eit->second, *vit->second);

        erange[0] += topo_num_elements;
    }

    // We now have vectors of element ids that we need to extract from each topo.
    // We also have sets of vertex ids that we need to extract from each coordset.

    conduit::Node *retval = new conduit::Node;
    conduit::Node &n_output = *retval;

// HEY, a concern I have about this way as opposed to making each selection do the
//      extract is that this way makes the output unstructured all the time. With
//      making the logical selection do the extraction, I can still output a logical
//      structured output for those topologies. This way can't really do that.

    // Create new coordsets that include the vertices that are relevant for the
    // selection.
    conduit::Node &n_new_coordsets = n_output["coordsets"];
    std::vector<index_t> all_selected_vertex_ids;
    for(index_t i = 0; i < n_coordsets.number_of_children(); i++)
    {
        const conduit::Node &n_coordset = n_coordsets[i];
        auto vit = coordset_vertex_ids.find(n_coordset.name());

        // TODO: avoid copying back to std::vector multiple times.
        std::vector<index_t> vertex_ids;
        index_t_set_to_vector(*vit->second, vertex_ids);

        // Build up a mapping of old to new vertices over all coordsets that we
        // can use to remap fields.
        for(auto it = vit->second->begin(); it != vit->second->end(); it++)
            all_selected_vertex_ids.push_back(*it);

        // Create the new coordset.
        create_new_explicit_coordset(n_coordset, vertex_ids, n_new_coordsets[n_coordset.name()]);
    }

    // Create new topologies containing the selected cells.
    conduit::Node &n_new_topos = n_output["topologies"];
    std::vector<index_t> all_selected_element_ids;
    for(index_t i = 0; i < n_topo.number_of_children(); i++)
    {
        const conduit::Node &n_this_topo = n_topo[i];
        auto eit = topo_element_ids.find(n_this_topo.name());
        if(!eit->second->empty())
        {
            const conduit::Node &n_coordset = n_this_topo["coordset"];
            auto vit = coordset_vertex_ids.find(n_coordset.name());
            if(vit != coordset_vertex_ids.end())
            {
                // Build up a mapping of old to new elements over all topos
                // can use to remap fields.
                for(size_t j = 0; j < eit->second->size(); j++)
                    all_selected_element_ids.push_back(eit->second->operator[](j));

                // TODO: avoid copying back to std::vector multiple times.
                std::vector<index_t> vertex_ids;
                index_t_set_to_vector(*vit->second, vertex_ids);

                create_new_unstructured_topo(n_this_topo, vertex_ids, 
                    *eit->second, n_new_topos[n_this_topo.name()]);
            }
        }
    }

    // Now that we've made new coordsets and topologies, make new fields.
    copy_fields(all_selected_vertex_ids, all_selected_element_ids,
                n_mesh, n_output,
                selections[idx]->preserve_mapping());

    return retval;
}

//---------------------------------------------------------------------------
void
partitioner::create_new_explicit_coordset(const conduit::Node &n_coordset,
    const std::vector<index_t> &vertex_ids, conduit::Node &n_new_coordset) const
{
    conduit::Node n_explicit;
    if(n_coordset["type"].as_string() == "uniform")
    {
        conduit::blueprint::mesh::coordset::uniform::to_explicit(n_coordset, n_explicit);

        auto axes = conduit::blueprint::mesh::utils::coordset::axes(n_explicit);
        const conduit::Node &n_values = n_explicit["values"];
        conduit::Node &n_new_values = n_new_coordset["values"];
        for(size_t i = 0; i < axes.size(); i++)
        {
            const conduit::Node &n_axis_values = n_values[axes[i]];
            conduit::Node &n_new_axis_values = n_new_values[axes[i]];
            slice_array(n_axis_values, vertex_ids, n_new_axis_values);
        }
    }
    else if(n_coordset["type"].as_string() == "rectilinear")
    {
        conduit::blueprint::mesh::coordset::rectilinear::to_explicit(n_coordset, n_explicit);

        auto axes = conduit::blueprint::mesh::utils::coordset::axes(n_explicit);
        const conduit::Node &n_values = n_explicit["values"];
        conduit::Node &n_new_values = n_new_coordset["values"];
        for(size_t i = 0; i < axes.size(); i++)
        {
            const conduit::Node &n_axis_values = n_values[axes[i]];
            conduit::Node &n_new_axis_values = n_new_values[axes[i]];
            slice_array(n_axis_values, vertex_ids, n_new_axis_values);
        }
    }
    else if(n_coordset["type"].as_string() == "explicit")
    {
        auto axes = conduit::blueprint::mesh::utils::coordset::axes(n_coordset);
        const conduit::Node &n_values = n_coordset["values"];
        conduit::Node &n_new_values = n_new_coordset["values"];
        for(size_t i = 0; i < axes.size(); i++)
        {
            const conduit::Node &n_axis_values = n_values[axes[i]];
            conduit::Node &n_new_axis_values = n_new_values[axes[i]];
            slice_array(n_axis_values, vertex_ids, n_new_axis_values);
        }
    }
}

//---------------------------------------------------------------------------
void
partitioner::create_new_unstructured_topo(const conduit::Node &n_topo,
    const std::vector<index_t> &element_ids,
    const std::vector<index_t> &vertex_ids,
    conduit::Node &n_new_topo) const
{
    if(n_topo["type"].as_string() == "uniform")
    {
        conduit::Node n_uns, cdest; // what is cdest?
        conduit::blueprint::mesh::topology::uniform::to_unstructured(n_topo, n_uns, cdest);
        unstructured_topo_from_unstructured(n_uns, element_ids, vertex_ids, n_new_topo);
    }
    else if(n_topo["type"].as_string() == "rectilinear")
    {
        conduit::Node n_uns, cdest; // what is cdest?
        conduit::blueprint::mesh::topology::rectilinear::to_unstructured(n_topo, n_uns, cdest);
        unstructured_topo_from_unstructured(n_uns, element_ids, vertex_ids, n_new_topo);
    }
    else if(n_topo["type"].as_string() == "structured")
    {
        conduit::Node n_uns, cdest; // what is cdest?
        conduit::blueprint::mesh::topology::structured::to_unstructured(n_topo, n_uns, cdest);
        unstructured_topo_from_unstructured(n_uns, element_ids, vertex_ids, n_new_topo);
    }
    else if(n_topo["type"].as_string() == "unstructured")
    {
        unstructured_topo_from_unstructured(n_topo, element_ids, vertex_ids, n_new_topo);
    }
}

//---------------------------------------------------------------------------
void
partitioner::unstructured_topo_from_unstructured(const conduit::Node &n_topo,
    const std::vector<index_t> &element_ids, const std::vector<index_t> &vertex_ids,
    conduit::Node &n_new_topo) const
{
    n_new_topo["type"].set("unstructured");
    n_new_topo["coordset"].set(n_topo["coordset"]);

    // vertex_ids contains the list of old vertex ids that our selection uses
    // from the old coordset. It can serve as a new to old map.

    std::map<index_t,index_t> old2new;
    for(size_t i = 0; i < vertex_ids.size(); i++)
        old2new[vertex_ids[i]] = static_cast<index_t>(i);

    const conduit::Node &n_conn = n_topo["elements/connectivity"];
// Conduit needs to_index_t_array() and as_index_t_ptr() methods.
    conduit::Node indices;
#ifdef CONDUIT_INDEX_32
    n_conn.to_unsigned_int_array(indices);
    auto iptr = indices.as_unsigned_int_ptr();
#else
    n_conn.to_unsigned_long_array(indices);
    auto iptr = indices.as_unsigned_long_ptr();
#endif
    conduit::blueprint::mesh::utils::ShapeType shape(n_topo);
    std::vector<index_t> new_conn;
    if(shape.is_poly())
    {
        // TODO:
    }
    else if(shape.is_polygonal())
    {
        // TODO:
    }
    else if(shape.is_polyhedral())
    {
        // TODO:
    }
    else
    {
        // Shapes are single types one after the next in the connectivity.
        auto nverts_in_shape = conduit::blueprint::mesh::utils::TOPO_SHAPE_INDEX_COUNTS[shape.id];
        for(size_t i = 0; i < element_ids.size(); i++)
        {
            auto elem_conn = iptr + element_ids[i] * nverts_in_shape;
            for(index_t j = 0; j < nverts_in_shape; j++)
                new_conn.push_back(old2new[elem_conn[j]]);
        }
    }

    n_new_topo["elements/shape"].set(n_topo["elements/shape"]);
    // TODO: Is there a better way to get the data into the node?
    n_new_topo["elements/connectivity"].set(new_conn);
}

//---------------------------------------------------------------------------
void
partitioner::execute(conduit::Node &output)
{
    // By this stage, we will have at least target selections spread across
    // the participating ranks. Now, we need to process the selections to
    // make chunks.
    std::vector<chunk> chunks;
    for(size_t i = 0; i < selections.size(); i++)
    {
        if(selections[i] == nullptr)
        {
            // We had a "null" selection so we'll take the whole mesh.
            chunks.push_back(chunk(meshes[i], false));
        }
        else
        {
            conduit::Node *c = extract(i, *meshes[i]);
c->print();
            chunks.push_back(chunk(c, true));
        }
    }

    // We need to figure out ownership and make sure each rank has the parts
    // that it needs to achieve "target" overall domains over all ranks. We
    // probably want to send/recv the data as binary blobs. Then deserialize
    // and assemble into combined grids.

    // Compute the destination rank and destination domain of each input
    // chunk present on this rank.
    std::vector<int> dest_rank, dest_domain;
    map_chunks(chunks, dest_rank, dest_domain);

    // Communicate chunks to the right destination ranks
    std::vector<chunk> chunks_to_assemble;
    std::vector<int> chunks_to_assemble_domains;
    communicate_chunks(chunks, dest_rank, dest_domain,
        chunks_to_assemble, chunks_to_assemble_domains);

    // Now that we have all the parts we need in chunks_to_assemble, combine
    // the chunks.
    std::set<int> unique_doms;
    for(size_t i = 0; i < chunks_to_assemble_domains.size(); i++)
        unique_doms.insert(chunks_to_assemble_domains[i]);

    if(!chunks_to_assemble.empty())
    {
        output.reset();

        for(auto dom = unique_doms.begin(); dom != unique_doms.end(); dom++)
        {
            // Get the chunks for this output domain.
            std::vector<const Node *> this_dom_chunks;
            for(size_t i = 0; i < chunks_to_assemble_domains.size(); i++)
            {
                if(chunks_to_assemble_domains[i] == *dom)
                    this_dom_chunks.push_back(chunks_to_assemble[i].mesh);
            }

            // Combine the chunks for this domain and add to output or to
            // a list in output.
            if(dom == unique_doms.begin())
            {
                // First time through.
                if(unique_doms.size() > 1)
                    combine(*dom, this_dom_chunks, output.append());
                else
                    combine(*dom, this_dom_chunks, output);
            }
            else
            {
                combine(*dom, this_dom_chunks, output.append());
            }
        }
    }

    // Clean up
    for(size_t i = 0; i < chunks.size(); i++)
        chunks[i].free();
    for(size_t i = 0; i < chunks_to_assemble.size(); i++)
        chunks_to_assemble[i].free();
}

//-------------------------------------------------------------------------
unsigned int
partitioner::starting_index(const std::vector<partitioner::chunk> &/*chunks*/)
{
    return 0;
}

//-------------------------------------------------------------------------
void
partitioner::map_chunks(const std::vector<partitioner::chunk> &chunks,
    std::vector<int> &dest_ranks,
    std::vector<int> &dest_domain)
{
    // All data stays on this rank in serial.
    dest_ranks.resize(chunks.size());
    for(size_t i =0 ; i < chunks.size(); i++)
        dest_ranks[i] = rank;

    // Determine average chunk size.
    std::vector<index_t> chunk_sizes;
    index_t total_len = 0;
    for(size_t i =0 ; i < chunks.size(); i++)
    {
        auto len = conduit::blueprint::mesh::topology::length(*chunks[i].mesh);
        total_len += len;
        chunk_sizes.push_back(len);
    }
    index_t len_per_target = total_len / target;

    int start_index = starting_index(chunks);
    if(chunks.size() == static_cast<size_t>(target))
    {
        // The number of chunks is the same as the target.
        for(size_t i =0 ; i < chunks.size(); i++)
            dest_domain.push_back(start_index + static_cast<int>(i));
    }
    else if(chunks.size() > static_cast<size_t>(target))
    {
        unsigned int domid = start_index;
        index_t total_len = 0;
        for(size_t i = 0; i < chunks.size(); i++)
        {
            total_len += chunk_sizes[i];
            if(total_len >= len_per_target && domid < target)
            {
                // Advance to the next domain index.
                total_len = 0;
                domid++;
            }

            dest_domain.push_back(domid);
        }
    }
    else
    {
        // The number of chunks is less than the target. Something is wrong!
        CONDUIT_ERROR("The number of chunks (" << chunks.size()
                      << ") is smaller than requested (" << target << ").");
    }
}

//-------------------------------------------------------------------------
void
partitioner::communicate_chunks(const std::vector<partitioner::chunk> &chunks,
    const std::vector<int> &/*dest_rank*/,
    const std::vector<int> &dest_domain,
    std::vector<partitioner::chunk> &chunks_to_assemble,
    std::vector<int> &chunks_to_assemble_domains)
{
    // In serial, communicating the chunks among ranks means passing them
    // back in the output arguments. We mark them as not-owned so we do not
    // double-free.
    for(size_t i = 0; i < chunks.size(); i++)
    {
        chunks_to_assemble.push_back(chunk(chunks[i].mesh, false));
        chunks_to_assemble_domains.push_back(dest_domain[i]);
    }
}

//-------------------------------------------------------------------------
void
partitioner::combine(int domain,
    const std::vector<const Node *> &inputs,
    Node &output)
{
    // NOTE: Some decisions upstream, for the time being, make all the chunks
    //       unstructured. We will try to relax that so we might end up
    //       trying to combine multiple uniform,rectilinear,structured
    //       topologies.

    // TODO: If all topologies for chunks are some type of structured and
    //       it looks like they all abut logically, check their coordsets
    //       so see if they line up in a compatible way too.
    //
    bool structured_compatible = false;
    if(structured_compatible)
    {
       // TODO: Make combined coordset and new structured topology

       // Add the combined result to output node.
    }
    else
    {
       // Determine names of all coordsets

       // Iterate over all coordsets and combine like-named coordsets into
       // new explicit coordset. Pass all points through an algorithm that
       // can combine the same points should they exist in multiple coordsets.
       //
       // for each coordset
       //     for each point in coordset
       //         new_pt_id = pointmap.get_id(point, tolerance)
       //              

       // Combine mapping information stored in chunks to assemble new field
       // that indicates original domain,pointid values for each point

       // Determine names of all topologies

       // Iterate over all topology names and combine like-named topologies
       // as new unstructured topology.

       // Combine mapping info stored in chunks to assemble new field that
       // indicates original domain,cellid values for each cell.

       // Use original point and cell maps to create new fields that combine
       // the fields from each source chunk. 
   
       // Add the combined result to output node.
    }
}

//-------------------------------------------------------------------------
/**
 @brief This class accepts a set of input meshes and repartitions them
        according to input options. This class subclasses the partitioner
        class to add some parallel functionality.
 */
#if 0
class parallel_partitioner : public partitioner
{
public:
    parallel_partitioner(MPI_Comm c);
    virtual ~parallel_partitioner();

    virtual long get_total_selections() const override;

    virtual void get_largest_selection(int &sel_rank, int &sel_index) const override;

protected:
    virtual void communicate_chunks(const std::vector<chunk> &chunks,
                                    const std::vector<int> &dest_rank,
                                    const std::vector<int> &dest_domain,
                                    std::vector<chunk> &chunks_to_assemble,
                                    std::vector<int> &chunks_to_assemble_domains) override;

private:
    MPI_Comm comm;
};

//---------------------------------------------------------------------------
parallel_partitioner::~parallel_partitioner(MPI_Comm c) : partitioner()
{
    comm = c;
    MPI_Comm_size(comm, &size);
    MPI_Comm_rank(comm, &rank);
}

//---------------------------------------------------------------------------
parallel_partitioner::~parallel_partitioner()
{
}

//---------------------------------------------------------------------------
long
parallel_partitioner::get_total_selections() const
{
    // Gather the number of selections on each rank.
    long nselections = static_cast<long>(selections.size());
    long ntotal_selections = nparts;
    MPI_Allreduce(&nselections, 1, MPI_LONG,
                  &ntotal_selections, 1, MPI_LONG, MPI_SUM, comm);

    return ntotal_selections;
}

//---------------------------------------------------------------------------
/**
 @note This method is called iteratively until we have the number of target
       selections that we want to make. We could do better by identifying
       more selections to split in each pass.
 */
void
parallel_partitioner::get_largest_selection(int &sel_rank, int &sel_index) const
{
    // Find largest selection locally.
    long largest_selection_size = 0;
    int  largest_selection_index = 0;
    for(size_t i = 0; i < selections.size(); i++)
    {
        long ssize = static_cast<long>(selections[i]->length());
        if(ssize > largest_selection_size)
        {
            largest_selection_size = ssize;
            largest_selection_index = static_cast<int>(i);
        }
    }

    // What's the largest selection across ranks?
    long global_largest_selection_size = 0;
    MPI_Allreduce(&largest_selection_size, 1, MPI_LONG,
                  &global_largest_selection_size, 1, MPI_LONG,
                  MPI_MAX, comm);

    // See if this rank has the largest selection.
    int rank_that_matches = -1, largest_rank_that_matches = -1;
    int local_index = -1;
    for(size_t i = 0; i < selections.size(); i++)
    {
        long ssize = static_cast<long>(selections[i]->length());
        if(ssize == global_largest_selection_size)
        {
            rank_that_matches = rank;
            local_index = -1;
        }
    }
    MPI_Allreduce(&rank_that_matches, 1, MPI_INT,
                  &largest_rank_that_matches, 1, MPI_INT,
                  MPI_MAX, comm);

    sel_rank = largest_rank_that_matches;
    if(sel_rank == rank)
        sel_index = local_index;
}

//-------------------------------------------------------------------------
void
parallel_partitioner::communicate_chunks(const std::vector<chunk> &chunks,
    const std::vector<int> &dest_rank,
    const std::vector<int> &dest_domain,
    std::vector<chunk> &chunks_to_assemble,
    std::vector<int> &chunks_to_assemble_domains)
{
    // TODO: send chunks to dest_rank if dest_rank[i] != rank.
    //       If dest_rank[i] == rank then the chunk stays on the rank.
    //
    //       Do sends/recvs to send the chunks as blobs among ranks.
    //
    //       Populate chunks_to_assemble, chunks_to_assemble_domains
}

#endif

//-------------------------------------------------------------------------
void
partition(const conduit::Node &n_mesh, const conduit::Node &options, conduit::Node &output)
{
    partitioner P;
    if(P.initialize(n_mesh, options))
    {
        P.split_selections();
        P.execute(output);
    }
}

}
//-----------------------------------------------------------------------------
// -- end conduit::blueprint::mesh --
//-----------------------------------------------------------------------------

#if 0//def CONDUIT_PARALLEL_PARTITION
// -- consider moving this into conduit_blueprint_mpi_mesh.cpp

//-----------------------------------------------------------------------------
// -- begin conduit::blueprint::mpi --
//-----------------------------------------------------------------------------
namespace mpi
{

//-----------------------------------------------------------------------------
// -- begin conduit::blueprint::mpi::mesh --
//-----------------------------------------------------------------------------
namespace mesh
{

//-------------------------------------------------------------------------
void
partition(const conduit::Node &n, const conduit::Node &options,
    conduit::Node &output, MPI_Comm comm)
{
    // Figure out the number of domains in the input mesh.
    auto ndoms = number_of_domains(mesh, comm);


    internal::partition(mesh, ndoms, options, output, comm);
}

}
//-----------------------------------------------------------------------------
// -- end conduit::blueprint::mpi::mesh --
//-----------------------------------------------------------------------------
}
//-----------------------------------------------------------------------------
// -- end conduit::blueprint::mpi --
//-----------------------------------------------------------------------------
#endif

}
//-----------------------------------------------------------------------------
// -- end conduit::blueprint --
//-----------------------------------------------------------------------------

}
//-----------------------------------------------------------------------------
// -- end conduit:: --
//-----------------------------------------------------------------------------