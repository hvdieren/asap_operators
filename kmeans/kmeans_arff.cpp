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

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <cilk/cilk_api.h>
#include <cerrno>
#include <cmath>
#include <limits>
#include <cassert>
#include <vector>
#include <type_traits>

#if SEQUENTIAL && PMC
#include <likwid.h>
#endif

#if !SEQUENTIAL
#include <cilk/cilk.h>
#include <cilk/reducer.h>
#else
#define cilk_sync
#define cilk_spawn
#define cilk_for for
#endif

#if TRACING
#include "tracing/events.h"
#include "tracing/events.cc"
#endif

// khere #include "stddefines.h"
#include <stddefines.h>

#define DEF_NUM_POINTS 100000
#define DEF_NUM_MEANS 100
#define DEF_DIM 3
#define DEF_GRID_SIZE 1000
#define DEF_NUM_THREADS 8

#define REAL_IS_INT 0
// typedef float real;
typedef double real;

int num_points; // number of vectors
int num_dimensions;         // Dimension of each vector
int num_clusters; // number of clusters
bool force_dense; // force a (slower) dense calculation
int max_iters; // maximum number of iterations
real * min_val; // min value of each dimension of vector space
real * max_val; // max value of each dimension of vector space
const char * infile = NULL; // input file
const char * outfile = NULL; // output file

#define CROAK(x)   croak(x,__FILE__,__LINE__)

inline void __attribute__((noreturn))
croak( const char * msg, const char * srcfile, unsigned lineno ) {
    const char * es = strerror( errno );
    std::cerr << srcfile << ':' << lineno << ": " << msg
	      << ": " << es << std::endl;
    exit( 1 );
}

struct point;

struct sparse_point {
    int * c;
    real * v;
    int nonzeros;
    int cluster;

    sparse_point() { c = NULL; v = NULL; nonzeros = 0; cluster = -1; }
    sparse_point(int *c, real* d, int nz, int cluster)
	: c(c), v(d), nonzeros(nz), cluster(cluster) { }
    sparse_point(const point&pt);
    
    bool normalize() {
	if( cluster == 0 ) {
	    std::cerr << "empty cluster...\n";
	    return true;
	} else {
#if VECTORIZED
	    v[0:nonzeros] /= (real)cluster;
#else
	    for(int i = 0; i < nonzeros; ++i)
		v[i] /= (real)cluster;
#endif
	    return false;
	}
    }

    void clear() {
#if VECTORIZED
	c[0:nonzeros] = (int)0;
	v[0:nonzeros] = (real)0;
#else
        for(int i = 0; i < nonzeros; ++i) {
	    c[i] = (int)0;
	    v[i] = (real)0;
	}
#endif
    }
    
    real sq_dist(point const& p) const;
    bool equal(point const& p) const;
    
    void dump() const {
        for(int j = 0; j < nonzeros; j++) {
#if REAL_IS_INT
	    printf("%d: %5ld ", (int)c[j], (long)v[j]);
#else
	    printf("%d: %6.4f ", (int)c[j], (double)v[j]);
#endif
	}
        printf("\n");
    }
};

std::ostream & operator << ( std::ostream & os, const sparse_point & sp ) {
    os << '{';
    for( int i=0, e=num_dimensions, j=0; i != e; ++i ) {
	real v = 0;
	if( i == sp.c[j] ) v = sp.v[j++];
	os << v;
	if( i+1 < e )
	    os << ", ";
    }
    os << '}';
    return os;
}

struct point
{
    real * d;
    real sumsq;
    int cluster;

    point() { d = NULL; cluster = -1; }
    point(real* d, int cluster) { this->d = d; this->cluster = cluster; }
    
    bool normalize() {
	if( cluster == 0 ) {
	    std::cerr << "empty cluster...\n";
	    return true;
	} else {
#if VECTORIZED
	    d[0:num_dimensions] /= (real)cluster;
#else
	    for(int i = 0; i < num_dimensions; ++i)
		d[i] /= (real)cluster;
#endif
	    return false;
	}
    }

    void update_sum_sq() {
	real ssq = 0;
        for (int i = 0; i < num_dimensions; i++) {
	    ssq += d[i] * d[i];
	}
	sumsq = ssq;
    }
    real get_sum_sq() const {
	return sumsq;
    }

    void clear() {
#if VECTORIZED
	d[0:num_dimensions] = (real)0;
#else
        for(int i = 0; i < num_dimensions; ++i)
	    d[i] = (real)0;
#endif
    }
    
#if VECTORIZED
    static unsigned real esqd( real a, real b ) {
	real diff = a - b;
need to adjust ...
	return diff * diff;
    }
    real sq_dist(point const& p) const {
	return __sec_reduce_add( esqd( d[0:num_dimensions],
				       p.d[0:num_dimensions] ) );
    }
#else
    real sq_dist(point const& p) const {
        real sum = 0;
        for (int i = 0; i < num_dimensions; i++) {
            real diff = d[i] - p.d[i];
	    // if( diff < (real)0 )
		// diff = -diff;
	    // diff = (diff - min_val[i]) / (max_val[i] - min_val[i] + 1);
            sum += diff * diff;
        }
        return sum;
    }
#endif
    
    void dump() const {
        for(int j = 0; j < num_dimensions; j++) {
#if REAL_IS_INT
	    printf("%5ld ", (long)d[j]);
#else
	    printf("%6.4f ", (double)d[j]);
#endif
	}
        printf("\n");
    }
    
    // For reduction of centre computations
    const point & operator += ( const point & pt ) {
#if VECTORIZED
	d[0:num_dimensions] += pt.d[0:num_dimensions];
#else
        for(int j = 0; j < num_dimensions; j++)
	    d[j] += pt.d[j];
#endif
	cluster += pt.cluster;
	return *this;
    }
    const point & operator += ( const sparse_point & pt ) {
// #if VECTORIZED
	// d[0:num_dimensions] += pt.d[0:num_dimensions];
// #else
        for(int j = 0; j < pt.nonzeros; j++)
	    d[pt.c[j]] += pt.v[j];
// #endif
	cluster += pt.cluster;
	return *this;
    }
};
typedef struct point Point;

sparse_point::sparse_point(const point&pt) {
    nonzeros=0;
    for( int i=0; i < num_dimensions; ++i )
	if( pt.d[i] != (real)0 )
	    ++nonzeros;

    c = new int[nonzeros];
    v = new real[nonzeros];

    int k=0;
    for( int i=0; i < num_dimensions; ++i )
	if( pt.d[i] != (real)0 ) {
	    c[k] = i;
	    v[k] = pt.d[i];
	    ++k;
	}
    assert( k == nonzeros );
}

real sparse_point::sq_dist(point const& p) const {
    real sum = 0;
#if 0
    int j=0;
    for( int i=0; i < num_dimensions; ++i ) {
	real diff;
	if( j < nonzeros && i == c[j] ) {
	    diff = v[j] - p.d[i];
	    ++j;
	} else
	    diff = p.d[i];
	sum += diff * diff;
    }
#else
    sum = p.get_sum_sq();
    for( int i=0; i < nonzeros; ++i ) { 
	sum += v[i] *  ( v[i] - real(2) * p.d[c[i]] );
	// assert( sum1 > real(0) );
    }
#endif
    // printf( "sum=%f sum1=%f\n", sum, sum1 );
    // assert( ( sum - sum1 ) / sum1 < 1e-3 );
    return sum;
}

bool sparse_point::equal(point const& p) const {
    int k=0;
    for( int i=0; i < nonzeros; ++i ) {
	while( k < c[i] ) {
	    if( p.d[k++] != (real)0 )
		return false;
	}
	if( p.d[k++] != v[i] )
	    return false;
    }
    while( k < num_dimensions ) {
	if( p.d[k++] != (real)0 )
	    return false;
    }
    return true;
}

class Centres {
    Point * centres;
    real * data;

public:
    Centres() {
	// Allocate backing store and initalize to zero
	data = new real[num_clusters * num_dimensions]();
	centres = new Point[num_clusters];
	for( int c=0; c < num_clusters; ++c ) {
	    centres[c] = Point( &data[c*num_dimensions], 0 );
	    centres[c].cluster = 0;
	}
    }
    ~Centres() {
	delete[] centres;
	delete[] data;
    }

    void clear() {
	for( int c=0; c < num_clusters; ++c ) {
	    centres[c].clear();
	    centres[c].cluster = 0;
	}
    }
    void add_point( Point * pt ) {
	int c = pt->cluster;
	Point & tgt = centres[c];
	for( int i=0; i < num_dimensions; ++i )
	    tgt.d[i] += pt->d[i];
	tgt.cluster++;
    }
    void add_point( sparse_point * pt ) {
	int c = pt->cluster;
	Point & tgt = centres[c];
	for( int i=0; i < pt->nonzeros; ++i )
	    tgt.d[pt->c[i]] += pt->v[i];
	tgt.cluster++;
    }

    void normalize( int c ) {
    	centres[c].normalize();
    }
    bool normalize() {
	bool modified = false;
	cilk_for( int c=0; c < num_clusters; ++c )
	    modified |= centres[c].normalize();
	return modified;
    }

    void update_sum_sq() {
	cilk_for( int c=0; c < num_clusters; ++c )
	    centres[c].update_sum_sq();
    }

    void select( const point * pts ) {
	for( int c=0; c < num_clusters; ) {
	    int pi = rand() % num_points;

	    // Check if we already have this point (may have duplicates)
	    bool incl = false;
	    for( int k=0; k < c; ++k ) {
		if( memcmp( centres[k].d, pts[pi].d,
			    sizeof(real) * num_dimensions ) ) {
		    incl = true;
		    break;
		}
	    }
	    if( !incl ) {
		for( int i=0; i < num_dimensions; ++i )
		    centres[c].d[i] = pts[pi].d[i];
		++c;
	    }
	}
    }
    void select( const sparse_point * pts ) {
	for( int c=0; c < num_clusters; ) {
	    int pi = rand() % num_points;

	    // Check if we already have this point (may have duplicates)
	    bool incl = false;
	    for( int k=0; k < c; ++k ) {
		if( pts[pi].equal( centres[k] ) ) {
		    incl = true;
		    break;
		}
	    }
	    if( !incl ) {
		centres[c].clear();
		for( int i=0; i < pts[pi].nonzeros; ++i )
		    centres[c].d[pts[pi].c[i]] = pts[pi].v[i];
		++c;
	    }
	}
    }

    template<typename DSPoint>
    real within_sse( DSPoint * points ) {
	real sse = 0;
	for( int i=0; i < num_points; ++i ) {
	    sse += points[i].sq_dist( centres[points[i].cluster] );
	}
	return sse;
    }

    const Point & operator[] ( int c ) const {
	return centres[c];
    }

    void reduce( Centres * cc ) {
	for( int c=0; c < num_clusters; ++c )
	    centres[c] += cc->centres[c];
    }

    void swap( Centres & c ) {
    	std::swap( data, c.data );
    	std::swap( centres, c.centres );
    }
};

#if !SEQUENTIAL
class centres_reducer {
    struct Monoid : cilk::monoid_base<Centres> {
	static void reduce( Centres * left, Centres * right ) {
#if TRACING
	    event_tracer::get().record( event_tracer::e_sreduce, 0, 0 );
#endif
	    left->reduce( right );
#if TRACING
	    event_tracer::get().record( event_tracer::e_ereduce, 0, 0 );
#endif
	}
    };

private:
    cilk::reducer<Monoid> imp_;

public:
    centres_reducer() : imp_() { }

    const Point & operator[] ( int c ) const {
	return imp_.view()[c];
    }

    void update_sum_sq() {
	imp_.view().update_sum_sq();
    }

    void swap( Centres & c ) {
	imp_.view().swap( c );
    }

    void add_point( Point * pt ) {
	imp_.view().add_point( pt );
    }
    void add_point( sparse_point * pt ) {
	imp_.view().add_point( pt );
    }
};

#else
typedef Centres centres_reducer;
#endif

template<typename DSPoint>
int kmeans_cluster(Centres & centres, DSPoint * points) {
    int modified = 0;

    centres_reducer new_centres;

    if( std::is_same<sparse_point,DSPoint>::value )
	centres.update_sum_sq();

#if GRANULARITY
    int nmap = std::min(num_points, 16) * 16;
    int g = std::max(1, (int)((double)(num_points+nmap-1) / nmap));
#pragma cilk grainsize = g
    cilk_for(int i = 0; i < num_points; i++) {
#else
    cilk_for(int i = 0; i < num_points; i++) {
#endif
#if TRACING
	event_tracer::get().record( event_tracer::e_smap, 0, 0 );
#endif
        //assign points to cluster
        real smallest_distance = std::numeric_limits<real>::max();
        int new_cluster_id = -1;
        for(int j = 0; j < num_clusters; j++) {
            //assign point to cluster with smallest total squared difference (for all d dimensions)
            real total_distance = points[i].sq_dist(centres[j]);
            if(total_distance < smallest_distance) {
                smallest_distance = total_distance;
                new_cluster_id = j;
            }
        }

        //if new cluster then update modified flag
        if(new_cluster_id != points[i].cluster)
        {
	    // benign race; works well. Alternative: reduction(|: modified)
            modified = 1;
            points[i].cluster = new_cluster_id;
        }

	new_centres.add_point( &points[i] );
#if TRACING
	event_tracer::get().record( event_tracer::e_emap, 0, 0 );
#endif
    }

#if TRACING
    event_tracer::get().record( event_tracer::e_synced, 0, 0 );
#endif

/*
    cilk_for(int i = 0; i < num_clusters; i++) {
	if( new_centres[i].cluster == 0 ) {
	    cilk_for(int j = 0; j < num_dimensions; j++) {
		new_centres[i].d[j] = centres[i].d[j];
	    }
	}
    }
*/

    // for(int i = 0; i < num_clusters; i++) {
	// std::cout << "in cluster " << i << " " << new_centres[i].cluster << " points\n";
    // }

    new_centres.swap( centres );
    centres.normalize();
    return modified;
}

void parse_args(int argc, char **argv)
{
    int c;
    extern char *optarg;
    
    // num_points = DEF_NUM_POINTS;
    num_clusters = DEF_NUM_MEANS;
    max_iters = 0;
    // num_dimensions = DEF_DIM;
    // grid_size = DEF_GRID_SIZE;
    
    while ((c = getopt(argc, argv, "c:i:o:m:d")) != EOF) 
    {
        switch (c) {
            // case 'd':
                // num_dimensions = atoi(optarg);
                // break;
	    case 'd':
		force_dense = true;
		break;
            case 'm':
                max_iters = atoi(optarg);
                break;
            case 'c':
                num_clusters = atoi(optarg);
                break;
            case 'i':
                infile = optarg;
                break;
            case 'o':
                outfile = optarg;
                break;
            // case 'p':
                // num_points = atoi(optarg);
                // break;
	    // case 's':
		// grid_size = atoi(optarg);
		// break;
            case '?':
                printf("Usage: %s -d <vector dimension> -c <num clusters> -p <num points> -s <max value> -t <number of threads>\n", argv[0]);
                exit(1);
        }
    }
    
    if( num_clusters <= 0 )
	CROAK( "Number of clusters must be larger than 0." );
    if( !infile )
	CROAK( "Input file must be supplied." );
    if( !outfile )
	CROAK( "Output file must be supplied." );
    
    std::cerr << "Number of clusters = " << num_clusters << '\n';
    std::cerr << "Input file = " << infile << '\n';
    std::cerr << "Output file = " << outfile << '\n';
}

struct arff_file {
    std::vector<const char *> idx;
    std::vector<sparse_point> points;
    char * fdata;
    char * relation;
    real * minval, * maxval;
    bool sparse_data;

public:
    arff_file() : sparse_data(false) { }

    void read_sparse_file( const char * fname ) {
	struct stat finfo;
	int fd;

	if( (fd = open( fname, O_RDONLY )) < 0 )
	    CROAK( fname );
	if( fstat( fd, &finfo ) < 0 )
	    CROAK( "fstat" );

	uint64_t r = 0;
	fdata = new char[finfo.st_size+1];
	while( r < (uint64_t)finfo.st_size )
	    r += pread( fd, fdata + r, finfo.st_size, r );
	fdata[finfo.st_size] = '\0';

	close( fd );

	size_t * tcount = 0;

	// Now parse the data
	char * p = fdata, * q;
#define ADVANCE(pp) do { if( *(pp) == '\0' ) goto END_OF_FILE; ++pp; } while( 0 )
	do {
	    while( *p != '@' )
		ADVANCE( p );
	    ADVANCE( p );
	    if( !strncasecmp( p, "relation ", 9 ) ) {
		p += 9;
		while( *p == ' ' )
		    ADVANCE( p );
		relation = p;
		if( *p == '\'' ) { // scan until closing quote
		    ADVANCE( p );
		    while( *p != '\'' )
			ADVANCE( p );
		    ADVANCE( p );
		    ADVANCE( p );
		    *(p-1) = '\0';
		} else { // scan until space
		    while( !isspace( *p ) )
			ADVANCE( p );
		    ADVANCE( p );
		    *(p-1) = '\0';
		}
	    } else if( !strncasecmp( p, "attribute ", 10 ) ) {
		p += 10;
		// Isolate token
		while( isspace( *p ) )
		    ADVANCE( p );
		q = p;
		while( !isspace( *p ) )
		    ADVANCE( p );
		ADVANCE( p );
		*(p-1) = '\0';
		// Isolate type
		while( isspace( *p ) )
		    ADVANCE( p );
		char * t = p;
		while( !isspace( *p ) )
		    ADVANCE( p );
		ADVANCE( p );
		*(p-1) = '\0';
		if( strcmp( t, "numeric" ) ) {
		    std::cerr << "Warning: treating non-numeric attribute '"
			      << q << "' of type '" << t << "' as numeric\n";
		}
		idx.push_back( q );
	    } else if( !strncasecmp( p, "data", 4 ) ) {
		// From now on everything is data
		int ndim = idx.size();
		tcount = new size_t[ndim](); // zero init
		p += 4;

		do {
		    while( isspace(*p) )
			ADVANCE( p );
		    bool is_sparse = *p == '{';
		    if( is_sparse ) {
			ADVANCE( p );
			sparse_data = true;
		    }
		    if( is_sparse ) {
			int nonzeros = 0;
			real * v = new real[ndim];
			int * c = new int[ndim];
			unsigned long nexti = 0;
			do {
			    while( isspace( *p ) )
				ADVANCE( p );
			    unsigned long i = strtoul( p, &p, 10 );
			    while( isspace( *p ) )
				ADVANCE( p );
			    if( *p == '?' )
				CROAK( "missing data not supported" );
			    real vv = 0;
#if REAL_IS_INT
			    vv = strtoul( p, &p, 10 );
#else
			    vv = strtod( p, &p );
#endif
			    // coord[i-1] = v;
			    v[nonzeros] = vv;
			    c[nonzeros] = i; // -1;
			    ++nonzeros;

			    tcount[i-1]++;

			    while( isspace( *p ) && *p != '\n' )
				ADVANCE( p );
			    if( *p == ',' )
				ADVANCE( p );
			} while( *p != '}' && *p != '\n' );
			do {
			    ADVANCE( p );
			} while( isspace( *p ) );
			real * s_v = new real[nonzeros];
			int * s_c = new int[nonzeros];
			std::copy( v, v+nonzeros, s_v );
			std::copy( c, c+nonzeros, s_c );
			delete[] c;
			delete[] v;
			points.push_back( sparse_point( s_c, s_v, nonzeros, -1 ) );
		    } else {
			real * coord = new real[ndim](); // zero init
			unsigned long nexti = 0;
			do {
			    while( isspace( *p ) )
				ADVANCE( p );
			    if( *p == '?' )
				CROAK( "missing data not supported" );
			    real v = 0;
#if REAL_IS_INT
			    v = strtoul( p, &p, 10 );
#else
			    v = strtod( p, &p );
#endif
			    coord[nexti++] = v;
			    while( isspace( *p ) && *p != '\n' )
				ADVANCE( p );
			    if( *p == ',' )
				ADVANCE( p );
			} while( *p != '}' && *p != '\n' );
			do {
			    ADVANCE( p );
			} while( isspace( *p ) );
			CROAK( "not supporting dense input right now" );
			// points.push_back( point( coord, -1 ) );
		    }
		} while( *p != '\0' );
	    }
	} while( 1 );

    END_OF_FILE:
	int ndim = idx.size();
	minval = new real[ndim];
	maxval = new real[ndim];
	for( int i=0; i < ndim; ++i ) {
	    minval[i] = std::numeric_limits<real>::max();
	    maxval[i] = std::numeric_limits<real>::min();
	}
#if 0
	cilk_for( int i=0; i < ndim; ++i ) {
	    for( int j=0; j < points.size(); ++j ) {
		real v = points[j].d[i];
		if( minval[i] > v )
		    minval[i] = v;
		if( maxval[i] < v )
		    maxval[i] = v;
	    }
	    for( int j=0; j < points.size(); ++j ) {
		points[j].d[i] = (points[j].d[i] - minval[i])
		    / (maxval[i] - minval[i]+1);
	    }
	}
#endif
	// Sparse property
	size_t num_points = points.size();
	cilk_for( int i=0; i < ndim; ++i ) {
	    if( tcount[i] < num_points )
		minval[i] = 0;
	}

	for( int j=0; j < num_points; ++j ) {
	    const sparse_point & pt = points[j];
	    for( int i=0; i < pt.nonzeros; ++i ) {
		real v = pt.v[i];
		int  c = pt.c[i];
		if( minval[c] > v )
		    minval[c] = v;
		if( maxval[c] < v )
		    maxval[c] = v;
	    }
	}
	cilk_for( int j=0; j < num_points; ++j ) {
	    sparse_point & pt = points[j];
	    for( int i=0; i < pt.nonzeros; ++i ) {
		real &v = pt.v[i];
		int   c = pt.c[i];
		if( minval[c] != maxval[c] ) {
		    v = (v - minval[c]) / (maxval[c] - minval[c]+1);
		} else {
		    v = (real)1;
		}
	    }
	}
	delete[] tcount;

	std::cerr << "@relation: " << relation << "\n";
	std::cerr << "@attributes: " << idx.size() << "\n";
	std::cerr << "@points: " << points.size() << "\n";
    }
};

int main(int argc, char **argv)
{
    struct timespec begin, end;
    struct timespec veryStart, veryEnd;

    srand( time(NULL) );

    get_time( begin );
    get_time( veryStart );

    //read args
    parse_args(argc,argv);

    std::cerr << "Available threads: " << __cilkrts_get_nworkers() << "\n";

#if SEQUENTIAL && PMC
    LIKWID_MARKER_INIT;
#endif // SEQUENTIAL && PMC

#if TRACING
    event_tracer::init();
#endif
    
    arff_file arff_data;
    arff_data.read_sparse_file( infile );
    num_dimensions = arff_data.idx.size();
    num_points = arff_data.points.size();
    min_val = arff_data.minval;
    max_val = arff_data.maxval;

    get_time (end);
    print_time("input", begin, end);

    get_time (begin);

    // for reproducibility
    srand(1);

    // allocate memory
    // get points
    sparse_point * points = &arff_data.points[0];

    // get means
    Centres centres;

    for( int i=0; i < num_points; ++i ) {
	points[i].cluster = rand() % num_clusters;
	centres.add_point( &points[i] );
    }
    centres.normalize();

    // for(int i = 0; i < num_clusters; i++) {
	// std::cout << "in cluster " << i << " " << centres[i].cluster << " points\n";
    // }

    get_time (end);
    print_time("initialize", begin, end);

    printf("KMeans: Calling MapReduce Scheduler\n");

    get_time (begin);        
    // keep re-clustering until means stabilise (no points are reassigned
    // to different clusters)
#if SEQUENTIAL && PMC
    LIKWID_MARKER_START("mapreduce");
#endif // SEQUENTIAL && PMC
    int niter = 1;

    // for( size_t i=0; i < num_points; ++i )
	// std::cout << points[i] << std::endl;

    while(kmeans_cluster(centres, points)) {
	if( ++niter >= max_iters && max_iters > 0 )
	    break;
	centres.update_sum_sq();
	fprintf( stdout, "within cluster SSE: %11.4lf\n", centres.within_sse( &points[0] ) );
    }
#if 0
    if( arff_data.sparse_data && !force_dense ) {
	// First build sparse representation
	std::vector<sparse_point> spoints;
	spoints.reserve( num_points );
	for( int i=0; i < num_points; ++i )
	    spoints.push_back( sparse_point( points[i] ) );

	// centres.update_sum_sq();
	// fprintf( stdout, "within cluster SSE: %11.4lf\n", centres.within_sse( &points[0] ) );

	while(kmeans_cluster(centres, &spoints[0])) {
	    if( ++niter >= max_iters && max_iters > 0 )
		break;
	    // centres.update_sum_sq();
	    // fprintf( stdout, "within cluster SSE: %11.4lf\n", centres.within_sse( &spoints[0] ) );
	}

	for( int i=0; i < num_points; ++i ) {
	    points[i].cluster = spoints[i].cluster; // copy result
	    delete[] spoints[i].c;
	    delete[] spoints[i].v;
	}
    } else {
	while(kmeans_cluster(centres, points)) {
	    if( ++niter >= max_iters && max_iters > 0 )
		break;
	}
    }
#endif
    get_time (end);        
#if SEQUENTIAL && PMC
    LIKWID_MARKER_STOP("mapreduce");
#endif // SEQUENTIAL && PMC

    print_time("library", begin, end);

    get_time (begin);

    //print means
    printf("KMeans: MapReduce Completed\n");  
#if 0
    printf("\n\nFinal means:\n");
    for(int i = 0; i < num_clusters; i++)
	centres[i].dump();
#endif
    fprintf( stdout, "sparse? %s\n",
	     ( arff_data.sparse_data && !force_dense ) ? "yes" : "no" );
    fprintf( stdout, "iterations: %d\n", niter );

/*
    real sse = 0;
    for( int i=0; i < num_points; ++i ) {
	sse += centres[points[i].cluster].sq_dist( points[i] );
    }
    fprintf( stdout, "within cluster sum of squared errors: %11.4lf\n", sse );
*/
    centres.update_sum_sq();
    fprintf( stdout, "within cluster SSE: %11.4lf\n", centres.within_sse( points ) );

    FILE * outfp = fopen( outfile, "w" );
    if( !outfp )
	CROAK( "cannot open output file for writing" );

    fprintf( outfp, "%37s\n", "Cluster#" );
    fprintf( outfp, "%-16s", "Attribute" );
    fprintf( outfp, "%10s", "Full Data" );
    for( int i=0; i < num_clusters; ++i )
	fprintf( outfp, "%11d", i );
    fprintf( outfp, "\n" );

    char buf[32];
    sprintf( buf, "(%d)", num_points );
    fprintf( outfp, "%26s", buf );
    for( int i=0; i < num_clusters; ++i ) {
	sprintf( buf, "(%d)", centres[i].cluster );
	fprintf( outfp, "%11s", buf );
    }
    fprintf( outfp, "\n" );

    fprintf( outfp, "================" );
    fprintf( outfp, "==========" );
    for( int i=0; i < num_clusters; ++i )
	fprintf( outfp, "===========" );
    fprintf( outfp, "\n" );

    for( int i=0; i < num_dimensions; ++i ) {
	fprintf( outfp, "%-16s", arff_data.idx[i] );
#if REAL_IS_INT
#error not yet implemented
#else
	real s = 0;
/*
	for( int j=0; j < num_points; ++j )
	    s += points[j].d[i];
*/
	for( int k=0; k < num_clusters; ++k )
	    s += centres[k].d[i];
	s /= (real)num_points;
	s = min_val[i] + s * (max_val[i] - min_val[i] + 1);
	fprintf( outfp, "%10.4lf", s );
#endif
	for( int k=0; k < num_clusters; ++k ) {
#if REAL_IS_INT
#error not yet implemented
#else
	    real s = 0;
/*
	    for( int j=0; j < num_points; ++j )
		if( points[j].cluster == k )
		    s += points[j].d[i];
*/
	    s = centres[k].d[i];
	    s /= (real)centres[k].cluster;
	    s = min_val[i] + s * (max_val[i] - min_val[i] + 1);
	    fprintf( outfp, "%11.4lf", s );
#endif
	}
	fprintf( outfp, "\n" );
    }

    //free memory
    // delete[] points; -- done in arff_file
    // oops, not freeing points[i].d 

    get_time (end);
    print_time("finalize", begin, end);
    print_time("complete time", veryStart, end);

#if TRACING
    event_tracer::destroy();
#endif
    
#if SEQUENTIAL && PMC
    LIKWID_MARKER_CLOSE;
#endif // SEQUENTIAL && PMC
    
    return 0;
}
