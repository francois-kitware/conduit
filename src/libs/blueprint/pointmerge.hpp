#ifndef POINT_MERGE_HPP
#define POINT_MERGE_HPP

// NOTE: THIS CLASS WILL BE MOVED

//-----------------------------------------------------------------------------
// conduit lib includes
//-----------------------------------------------------------------------------
#include "conduit.hpp"
#include "conduit_blueprint_exports.h"

//-----------------------------------------------------------------------------
// std lib includes
//-----------------------------------------------------------------------------
#include <cstddef>
#include <cmath>
#include <utility>
#include <algorithm>

//-----------------------------------------------------------------------------
// -- begin conduit:: --
//-----------------------------------------------------------------------------
namespace conduit
{

//-----------------------------------------------------------------------------
// -- begin conduit::blueprint --
//-----------------------------------------------------------------------------
namespace blueprint
{

inline double distance2(const double A[3], const double B[3])
{
    const double dx = B[0] - A[0];
    const double dy = B[1] - A[1];
    const double dz = B[2] - A[2];
    return dx*dx + dy*dy + dz*dz;
}

inline double distance(const double A[3], const double B[3])
{
    return std::sqrt(distance2(A, B));
}

template<size_t Dimension, typename record>
class kdtree
{
public:
    template<typename Precision, size_t D>
    struct kdnode
    {
        using node = kdnode<Precision, D>;
        Precision loc[D];
        node *left{nullptr};
        node *right{nullptr};
        record r;
    };
    using node = kdnode<double, Dimension>;
    using pair = std::pair<node*, bool>;

    kdtree() = default;
    ~kdtree()
    { 
        const auto lambda = [](node *node, unsigned int)
        {
            delete node;
        };
        if(root) { traverse_lrn(lambda, root); }
    }

    pair insert(const record &r, const double loc[Dimension])
    {
        auto p = insert(root, 0, loc, r);
        if(!root) 
        {
            root = p.first;
        }
        return p;
    }

    size_t size()  const { return nnodes; }
    size_t depth() const { return tree_depth; }

    void set_tolerance(double t) { tolerance = t; }
    double get_tolerance() const { return tolerance; }

    template<typename Func>
    void traverse(Func &&func) 
    {
        if(root) { traverse_lnr(func, root); }
    }

    void print_tree(std::ostream &oss)
    {
        traverse([&oss](node *n, unsigned int depth){
            for(auto i = 0u; i < depth; i++) oss << "   ";
            oss << depth << "(" << n->loc[0] << " " << n->loc[1] << " " << n->loc[2] << ")\n";
        });
        std::flush(oss);
    }

private:
    node *create_node(const double loc[Dimension], const record &r)
    {
        node *newnode = new node;
        for(auto i = 0u; i < Dimension; i++)
        {
            newnode->loc[i] = loc[i];
        }
        newnode->left = nullptr;
        newnode->right = nullptr;
        newnode->r = r;
        nnodes++;
        return newnode;
    }

    pair insert(node *current, unsigned int depth, const double loc[Dimension], const record &r)
    {
        if(!current)
        {
            return {create_node(loc, r), true};
        }

        // Determine if we are in the tolerance threshold
        const double dist = std::abs(distance(current->loc, loc));
        if(dist < tolerance)
        {
            return {current, false};
        }

        const unsigned int dim = depth % Dimension;
        const unsigned int next_depth = depth + 1;
        tree_depth = std::max(tree_depth, static_cast<size_t>(next_depth));
        if(loc[dim] < current->loc[dim])
        {
            auto p = insert(current->left, next_depth, loc, r);
            if(!current->left) current->left = p.first;
            return p;
        }
        else
        {
            auto p = insert(current->right, next_depth, loc, r);
            if(!current->right) current->right = p.first;
            return p;
        }
    }

    template<typename Func>
    void traverse_lnr(Func &&func, node *node, unsigned int depth = 0)
    {
        if(node->left) { traverse_lnr(func, node->left, depth + 1); }
        func(node, depth);
        if(node->right) { traverse_lnr(func, node->right, depth + 1); }
    }

    template<typename Func>
    void traverse_lrn(Func &&func, node *node, unsigned int depth = 0)
    {
        if(node->left) { traverse_lrn(func, node->left, depth + 1); }
        if(node->right) { traverse_lrn(func, node->right, depth + 1); }
        func(node, depth);
    }

    // Keep track of tree performance
    size_t nnodes{0u};
    size_t tree_depth{0u};

    node *root{nullptr};
    double tolerance{0.};
};

class CONDUIT_BLUEPRINT_API point_merge
{
public:
    enum class coord_system
    {
        cartesian,
        cylindrical,
        spherical
    };

    void execute(const std::vector<conduit::Node *> coordsets, double tolerance,
                Node &output);

private:
    struct record
    {
        using index_t = conduit_index_t;
        index_t orig_domain;
        index_t orig_id;
    };

    void iterate_coordinates(index_t domain_id, coord_system cs,
        const Node *xnode, const Node *ynode, const Node *znode);

    void insert(index_t dom_id, index_t pid, coord_system system,
        double x, double y, double z);

    using merge_tree = kdtree<3, record>;
    merge_tree merge;
    coord_system out_system;
};

//-----------------------------------------------------------------------------
}
//-----------------------------------------------------------------------------
// -- end conduit::blueprint --
//-----------------------------------------------------------------------------


}
//-----------------------------------------------------------------------------
// -- end conduit:: --
//-----------------------------------------------------------------------------

// cpp
#include <cmath>
#include <conduit_node.hpp>

//-----------------------------------------------------------------------------
// -- begin conduit:: --
//-----------------------------------------------------------------------------
namespace conduit
{

//-----------------------------------------------------------------------------
// -- begin conduit::blueprint --
//-----------------------------------------------------------------------------
namespace blueprint
{

inline void 
point_merge::execute(const std::vector<Node *> coordsets, double tolerance,
                    Node &output)
{
    merge.set_tolerance(tolerance);

    // Determine best output coordinate system
    out_system = coord_system::cartesian;
    const index_t ncoordsets = coordsets.size();
    for(index_t i = 0u; i < ncoordsets; i++)
    {
        const Node *coordset = coordsets[i]->fetch_ptr("values");
        // TODO: Check if it is explicit
        coord_system cs = coord_system::cartesian;
        const Node *xnode = coordset->fetch_ptr("x");
        const Node *ynode = nullptr, *znode = nullptr;
        if(xnode)
        {
            // Cartesian
            ynode = coordset->fetch_ptr("y");
            znode = coordset->fetch_ptr("z");
        }
        else if((xnode = coordset->fetch_ptr("r")))
        {
            if((ynode = coordset->fetch_ptr("z")))
            {
                // Cylindrical
                cs = coord_system::cylindrical;
            }
            else if((ynode = coordset->fetch_ptr("theta")))
            {
                // Spherical
                cs = coord_system::spherical;
                znode = coordset->fetch_ptr("phi");
            }
            else
            {
                // ERROR: Invalid coordinate system
                continue;
            }
        }
        // Is okay with nodes being nullptr
        iterate_coordinates(i, cs, xnode, ynode, znode);
    }

    // Iterate the record map
    const auto npoints = merge.size();
    output.reset();
    auto &coordset = output.add_child("coordsets");
    auto &coords = coordset.add_child("coords");
    coords["type"] = "explicit";
    auto &values = coords.add_child("values");
    
    // create x,y,z
    Schema s;
    const index_t stride = sizeof(double) * 3;
    const index_t size = sizeof(double);
    s["x"].set(DataType::c_double(npoints,0,stride));
    s["y"].set(DataType::c_double(npoints,size,stride));
    s["z"].set(DataType::c_double(npoints,size*2,stride));
    
    // init the output
    values.set(s);
    double_array x_a = values["x"].value();
    double_array y_a = values["y"].value();
    double_array z_a = values["z"].value();
    index_t point_id = 0;
    merge.traverse([&point_id, &x_a, &y_a, &z_a](merge_tree::node *node, unsigned int) {
        x_a[point_id] = node->loc[0];
        y_a[point_id] = node->loc[1];
        z_a[point_id] = node->loc[2];
        point_id++;
    });
    merge.print_tree(std::cout);
    // output.print();
}

inline void
point_merge::iterate_coordinates(index_t domain_id, coord_system cs,
    const Node *xnode, const Node *ynode, const Node *znode)
{
    if(xnode && ynode && znode)
    {
        // 3D
        const auto xtype = xnode->dtype();
        const auto ytype = ynode->dtype();
        const auto ztype = znode->dtype();
        // TODO: Handle different types
        auto xarray = xnode->as_double_array();
        auto yarray = ynode->as_double_array();
        auto zarray = znode->as_double_array();
        const index_t N = xarray.number_of_elements();
        for(index_t i = 0; i < N; i++)
        {
            insert(domain_id, i, cs, xarray[i], yarray[i], zarray[i]);
        }
    }
    else if(xnode && ynode)
    {
        // 2D
        const auto xtype = xnode->dtype();
        const auto ytype = ynode->dtype();
        // TODO: Handle different types
        auto xarray = xnode->as_double_array();
        auto yarray = ynode->as_double_array();
        const index_t N = xarray.number_of_elements();
        for(index_t i = 0; i < N; i++)
        {
            insert(domain_id, i, cs, xarray[i], yarray[i], 0.);
        }
    }
    else if(xnode)
    {
        // 1D
        const auto xtype = xnode->dtype();
        // TODO: Handle different types
        auto xarray = xnode->as_double_array();
        const index_t N = xarray.number_of_elements();
        for(index_t i = 0; i < N; i++)
        {
            insert(domain_id, i, cs, xarray[i], 0., 0.);
        }
    }
    else
    {
        // ERROR! No valid nodes passed.
    }
}

inline void 
point_merge::insert(index_t dom_id, index_t pid, coord_system system,
                    double x, double y, double z)
{
    double point[3] = {x, y, z};
    record r{dom_id, pid};

    // TODO: Handle coordinate systems
    merge.insert(r, point);
}

//-----------------------------------------------------------------------------
}
//-----------------------------------------------------------------------------
// -- end conduit::blueprint --
//-----------------------------------------------------------------------------


}
//-----------------------------------------------------------------------------
// -- end conduit:: --
//-----------------------------------------------------------------------------




#endif