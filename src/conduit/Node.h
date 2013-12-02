///
/// file: Node.h
///

#ifndef conduit_node_h__
#define conduit_node_h__

#include "Core.h"
#include "Type.h"
#include <map>
#include <vector>
#include <string>
#include <sstream>

#include "rapidjson/document.h"

namespace conduit
{

class Node
{
public:
    Node(); // empty node
    Node(const Node &node);
    explicit Node(const DataType &dtype);
    Node(void *data, const std::string &schema);
    Node(void *data, const Node *schema);
    Node(void *data, const DataType &dtype);

    explicit Node(uint32  data);
    explicit Node(float64 data);

    explicit Node(const std::vector<uint32>  &data);
    explicit Node(const std::vector<float64>  &data);

    virtual  ~Node();

    void set(const Node& data);
    void set(const DataType &data);

    void set(uint32 data);
    void set(float64 data);

    void set(const std::vector<uint32>  &data);
    void set(const std::vector<float64> &data);

    void set(void* data, const Node* schema);
    void set(void* data, const DataType &dtype);
    
    template<typename T>
    void setpp(T data);
    
    template<typename T>
    T getpp(void) const;
    
    template<typename T>
    T getpp(void *data) const;

    Node &operator=(const Node &node);

    Node &operator=(DataType dtype);

    Node &operator=(uint32 data);
    Node &operator=(float64 data);

    Node &operator=(const std::vector<uint32>  &data);
    Node &operator=(const std::vector<float64>  &data);

    index_t total_bytes() const;

    std::string schema() const;
    void        schema(std::ostringstream &oss) const;

    void        serialize(std::vector<uint8> &data) const;
    void        serialize(uint8 *data, index_t curr_offset) const;

    const DataType    &dtype() const { return m_dtype;}
    // bool              operator==(const Node &obj) const;
    // TODO: we will likly need const variants of these methods

    Node             &fetch(const std::string &path);

    template<typename TYPE>
    void             push_back(TYPE data);

    bool             has_path(const std::string &path) const;
    void             paths(std::vector<std::string> &paths,bool expand=false) const;

    Node             &operator[](const std::string &path)
                      {return fetch(path);}
    Node             &operator[](const index_t idx)
                      {return m_list_data[idx];}

    index_t          to_integer() const;
    float64          to_real()    const;

    uint32           as_uint32()  const { return *((uint32*)element_pointer(0));}
    float64          as_float64() const { return *((float64*)element_pointer(0));}

    uint32          *as_uint32_ptr()   { return (uint32*)element_pointer(0);}
    float64         *as_float64_ptr()  { return (float64*)element_pointer(0);}

    
private:
   

    void             init(const DataType &dtype);
    void             cleanup(); //dalloc 
    
    void             walk_schema(void *data, 
                                 const std::string &schema);

    void             walk_schema(void *data, 
                                 const rapidjson::Value &jvalue, 
                                 index_t curr_offset);
    

    // for value types
    index_t          element_index(index_t   idx) const;
    void            *element_pointer(index_t idx)
                     {return static_cast<char*>(m_data) + element_index(idx);};
    const void      *element_pointer(index_t idx) const 
                     {return static_cast<char*>(m_data) + element_index(idx);};
    bool             compatible_storage(const DataType& type);


    void     *m_data;
    bool      m_alloced;
    DataType  m_dtype;

    // for true nodes
    std::map<std::string, Node> &entries();
    std::vector<Node>           &list();

    const std::map<std::string, Node> &entries() const;
    const std::vector<Node>           &list() const;


    std::vector<Node> m_list_data;
    std::map<std::string, Node> m_entries;
};

// TODO: Explicit temp inst in the c file. 

template<typename TYPE>
void Node::push_back(TYPE data)
{
   if (m_dtype.id() != DataType::LIST_T) {
       m_dtype.reset(DataType::LIST_T);
   }
   m_list_data.push_back(Node(data));
}

template<typename T>
void Node::setpp(T data)
{
   // TODO check for compatible, don't always re-init
   init(DataType::type_id_to_datatype(DataType::Traits<T>::data_type));
   *((T*)m_data) = data;
}

template<typename T>
T Node::getpp(void) const
{
   // TODO some kind of checking here.. is m_data valid? right type?
   return *(T const *)( ((char const *)m_data) + m_dtype.element_index(0));
}

template<typename T>
T Node::getpp(void *data) const
{
   // TODO some kind of checking here.. is m_data valid? right type?
   return *(T const *)( ((char const *)data) + m_dtype.element_index(0));
}


}


#endif
