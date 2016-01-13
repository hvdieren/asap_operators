#include <unistd.h>

#include <iostream>
#include <fstream>
#include <deque>
#include <unordered_map>

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
#include "asap/kmeans.h"

#include <stddefines.h>

#define DEF_NUM_MEANS 8

size_t num_clusters = DEF_NUM_MEANS;
size_t max_iters = 0;
char const * indir = nullptr;
char const * outfile = nullptr;

static void help(char *progname) {
    std::cout << "Usage: " << progname
	      << " -i <indir> -o <outfile> -c <numclusters> [-m <maxiters>]\n";
}

static void parse_args(int argc, char **argv) {
    int c;
    extern char *optarg;
    
    while ((c = getopt(argc, argv, "i:o:c:m:")) != EOF) {
        switch (c) {
	case 'i':
	    indir = optarg;
	    break;
	case 'o':
	    outfile = optarg;
	    break;
	case 'm':
	    max_iters = atoi(optarg);
	    break;
	case 'c':
	    num_clusters = atoi(optarg);
	    break;
	case '?':
	    help(argv[0]);
	    exit(1);
        }
    }
    
    if( num_clusters <= 0 )
	fatal( "Number of clusters must be larger than 0." );
    if( !indir )
	fatal( "Input directory must be supplied." );
    if( !outfile )
	fatal( "Output file must be supplied." );
    
    std::cerr << "Number of clusters = " << num_clusters << '\n';
    std::cerr << "Maximum iterations = " << max_iters << '\n';
    std::cerr << "Input directory = " << indir << '\n';
    std::cerr << "Output file = " << outfile << '\n';
}

int main(int argc, char **argv) {
    struct timespec begin, end;
    struct timespec veryStart;

    srand( time(NULL) );

    get_time( begin );
    veryStart = begin;

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
    typedef asap::word_map<std::unordered_map<const char *, size_t, asap::text::charp_hash, asap::text::charp_eql>, asap::word_bank_pre_alloc> word_map_type;
    typedef asap::word_list<std::vector<std::pair<const char * const, size_t>>, asap::word_bank_pre_alloc> word_list_type;

    typedef asap::sparse_vector<size_t, float, false,
				asap::mm_no_ownership_policy>
	vector_type;
    typedef asap::word_map<std::unordered_map<const char *,
				    asap::appear_count<size_t,
						       typename vector_type::index_type>,
				    asap::text::charp_hash, asap::text::charp_eql>,
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
	    asap::word_catalog( std::string(*std::next(dir_list.cbegin(),i)),
				wmap ); // catalog[i] );
	    // Convert file's catalog to a (sorted) list of pairs
	    catalog[i].reserve( wmap.size() );    // avoid re-allocations
	    catalog[i].insert( std::move(wmap) ); // move out wmap contents
	} // delete wmap

	// std::cerr << ": " << catalog[i].size() << " words\n";
	// Reading from std::vector rather than std::map should be faster...
	// Validated: about 10% on word count, 20% on TF/IDF, 16 threads
	allwords.count_presence( catalog[i] );
    }
    get_time (end);
    print_time("word count", begin, end);

    get_time( begin );
    typedef asap::data_set<vector_type, word_map_type2> data_set_type;
    // TODO: consider linearising the word_map to a word_list with exchanged
    //       word_bank in order to avoid storing the ID? Problem: lookup
    //       during TF/IDF computation
    // TODO: infer word_map_type2 from word_map_type* in template definition?
    // TODO: construct aggregate word_map_type2 during wc loop above
    std::shared_ptr<word_map_type2> allwords_ptr
	= std::make_shared<word_map_type2>();
    allwords_ptr->swap( allwords.get_value() );

    data_set_type data_set
	= asap::tfidf<vector_type, std::vector<word_list_type>::const_iterator,
		      word_map_type2>(
			  catalog.cbegin(), catalog.cend(), allwords_ptr,
			  false ); // catalogs are not sorted!
    get_time( end );
    print_time("TF/IDF", begin, end);
    std::cerr << "TF/IDF number of words: " << data_set.get_dimensions()
	      << "\nTF/IDF number of files: " << data_set.get_num_points()
	      << std::endl;

    // Normalize data for improved clustering results
    get_time( begin );
    std::vector<std::pair<float, float>> extrema
	= asap::normalize( data_set );
    get_time( end );
    print_time("normalize", begin, end);

    // K-means clustering
    get_time( begin );
    auto kmeans_op = asap::kmeans( data_set, num_clusters, max_iters );
    get_time( end );
    print_time("K-Means", begin, end);
    std::cerr << "K-Means iterations: " << kmeans_op.num_iterations()
	      << "\nK-Means within-cluster SSE: " << kmeans_op.within_sse()
	      << std::endl;

    // Unscale data
    get_time( begin );
    asap::denormalize( extrema, data_set );
    get_time( end );        
    print_time("denormalize", begin, end);

    get_time( begin );
    std::ofstream of( outfile, std::ios_base::out );
    kmeans_op.output( of );
    of.close();
    get_time( end );
    print_time("output", begin, end);

    print_time("complete time", veryStart, end);

    return 0;
}
