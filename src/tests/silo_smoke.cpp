/*****************************************************************************
* Copyright (c) 2014, Lawrence Livermore National Security, LLC
* Produced at the Lawrence Livermore National Laboratory. 
* 
* All rights reserved.
* 
* This source code cannot be distributed without further review from 
* Lawrence Livermore National Laboratory.
*****************************************************************************/

///
/// file: silo_smoke.cpp
///

#include <silo.h>
#include <iostream>
#include "gtest/gtest.h"


TEST(silo_smoke_test_case, silo_smoke)
{


    DBfile *dbfile = DBCreate("silo_smoke_test.silo", 0, DB_LOCAL, "test", DB_HDF5);
    std::string twrite = "test_string";
    int twrite_len = twrite.size()+1;
    DBWrite (dbfile, "tdata", twrite.c_str(), &twrite_len, 1, DB_CHAR);
    DBClose(dbfile);
    
    dbfile = DBOpen("silo_smoke_test.silo", DB_HDF5, DB_READ);
    
    
    int tread_len  = DBGetVarLength(dbfile, "tdata");
    char  *tread = new char[tread_len];
    DBReadVar(dbfile, "tdata", tread);
    DBClose(dbfile);
    
    EXPECT_EQ(twrite,std::string(tread));
    delete [] tread;
}