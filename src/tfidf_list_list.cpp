/* -*-C++-*-
*/
/*
 * Copyright 2016 EU Project ASAP 619706.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include <unistd.h>

#include <iostream>
#include <fstream>
#include <deque>

#include <cilk/cilk.h>
#include <cilk/reducer.h>
#include <cilk/cilk_api.h>

#include "asap/utils.h"
#include "asap/arff.h"
#include "asap/dense_vector.h"
#include "asap/sparse_vector.h"
#include "asap/word_count.h"
#include "asap/normalize.h"
#include "asap/io.h"

#include <stddefines.h>

#define DEF_NUM_MEANS 8

char const * indir = nullptr;
char const * outfile = nullptr;
bool by_words = false;

static void help(char *progname) {
    std::cout << "Usage: " << progname << " -i <indir> -o <outfile>\n";
}

static void parse_args(int argc, char **argv) {
    int c;
    extern char *optarg;
    
    while ((c = getopt(argc, argv, "i:o:w")) != EOF) {
        switch (c) {
	case 'i':
	    indir = optarg;
	    break;
	case 'o':
	    outfile = optarg;
	    break;
	case 'w':
	    by_words = true;
	    break;
	case '?':
	    help(argv[0]);
	    exit(1);
        }
    }
    
    if( !indir )
	fatal( "Input directory must be supplied." );
    
    std::cerr << "Input directory = " << indir << '\n';
    std::cerr << "Output file = " << outfile << '\n';
    std::cerr << "TF/IDF by words = " << ( by_words ? "true\n" : "false\n" );
}

int main(int argc, char **argv) {
    struct timespec begin, end;
    struct timespec veryStart;

    srand( time(NULL) );

    get_time( begin );
    get_time( veryStart );

    // read args
    parse_args(argc,argv);

    std::cerr << "Available threads: " << __cilkrts_get_nworkers() << "\n";

    get_time (end);
    print_time("init", begin, end);

    // Directory listing
    get_time( begin );
    typedef asap::word_list<std::deque<const char*>, asap::word_bank_managed>
	directory_listing_type;
    directory_listing_type dir_list;
    asap::get_directory_listing( indir, dir_list );
    get_time (end);
    print_time("directory listing", begin, end);

    // word count
    get_time( begin );
    typedef asap::word_map<std::map<const char *, size_t, asap::text::charp_cmp>, asap::word_bank_pre_alloc> word_map_type;
    typedef asap::kv_list<std::vector<std::pair<const char *, size_t>>, asap::word_bank_pre_alloc> word_list_type;

    typedef asap::sparse_vector<size_t, float, false,
				asap::mm_no_ownership_policy>
	vector_type;
    typedef asap::kv_list<std::vector<std::pair<const char *,
	asap::appear_count<size_t,
			   typename vector_type::index_type>>>,
			    asap::word_bank_pre_alloc> word_list_type2;
    typedef asap::word_map<std::map<const char *,
				    asap::appear_count<size_t,
						       typename vector_type::index_type>,
				    asap::text::charp_cmp>,
			   asap::word_bank_pre_alloc> word_map_type2;
    size_t num_files = dir_list.size();
    std::vector<word_list_type> catalog;
    catalog.resize( num_files );

    asap::word_container_reducer<word_map_type2> allwords;
    cilk_for( size_t i=0; i < num_files; ++i ) {
	std::string filename = *std::next(dir_list.cbegin(),i);
	// std::cerr << "Read file " << filename;
	{
	    // Build up catalog for each file using a map
	    word_map_type wmap;
	    asap::word_catalog<word_map_type>( std::string(*std::next(dir_list.cbegin(),i)),
				wmap ); // catalog[i] );
	    // Convert file's catalog to a (sorted) list of pairs
	    catalog[i].reserve( wmap.size() );    // avoid re-allocations
	    catalog[i].insert( std::move(wmap) ); // move out wmap contents
	} // delete wmap

	// std::cerr << ": " << catalog[i].size() << " words\n";
	// Reading from std::vector rather than std::map should be faster...
	// Validated: about 10% on word count, 20% on TF/IDF, 16 threads

	// TODO: reduction through merge of vectors into vector would be
	//       faster: O(M+N) vs O(M logN)
	//       However, TF/IDF step is served better with map (although we
	//       could also do binary search in vector for lookups...)
	//       So options are:
	//        A. convert to map before TF/IDF. Since sorted, O(N)
	//        B. binary search in TF/IDF in vector
	//           If we did binary search in vector, we would not need to
	//           pre-compute unique IDs for the words but we could infer
	//           them while searching!
	//  ... Seems like the time increase here for merging sorted lists
	//      is much higher than the total time taken by TF/IDF below.
	allwords.count_presence( catalog[i] );
    }
    get_time (end);
    print_time("word count", begin, end);

    // TODO: revert word_bank such that all per-file catalogs use the "main"
    //       word_bank and use pointers into this word_bank. As such, during
    //       TF/IDF we do not need to use strcmp, but can use direct pointer
    //       comparison for the lookups.
    //       This, however, requires to re-sort or re-construct the allwords
    //       map such that is sorted by pointer rather than alphabetically.
    //       Most efficient on unordered_map as it simplifies the hash?

    get_time( begin );
    typedef asap::data_set<vector_type, word_list_type2, directory_listing_type> data_set_type;
    // TODO: consider linearising the word_map to a word_list with exchanged
    //       word_bank in order to avoid storing the ID? Problem: lookup
    //       during TF/IDF computation
    // TODO: infer word_list_type2 from word_map_type* in template definition?
    // TODO: construct aggregate word_list_type2 during wc loop above
    std::shared_ptr<word_list_type2> allwords_ptr
	= std::make_shared<word_list_type2>();
    allwords_ptr->insert( std::move(allwords.get_value()) );

    std::shared_ptr<directory_listing_type> dir_list_ptr
	= std::make_shared<directory_listing_type>();
    dir_list_ptr->swap( dir_list );

    get_time (end);
    print_time("convert to list", begin, end);

    get_time( begin );
    asap::internal::assign_ids( allwords_ptr->begin(), allwords_ptr->end() );
    data_set_type tfidf(
	by_words
	? asap::tfidf_by_words<vector_type>(
	    catalog.cbegin(), catalog.cend(), allwords_ptr, dir_list_ptr )
	: asap::tfidf<vector_type>(
	    catalog.cbegin(), catalog.cend(), allwords_ptr, *allwords_ptr,
	    dir_list_ptr , true, true)
	);
    get_time (end);
    print_time("TF/IDF", begin, end);

    get_time( begin );
    if( outfile )
	asap::arff_write( outfile, tfidf );
    get_time (end);
    print_time("output", begin, end);

    print_time("complete time", veryStart, end);

    return 0;
}
