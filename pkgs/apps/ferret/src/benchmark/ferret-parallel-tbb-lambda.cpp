#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <cass.h>
extern "C" {
#include <cass_timer.h>
}
extern "C" {
#include <../image/image.h>
}
#include "tpool.h"
#include "queue.h"

#include "tbb/task_scheduler_init.h"
#include "tbb/pipeline.h"

#include <stack>
#include <functional>

#define PIPELINE_WIDTH 1024

struct load_data {
	int width, height;
	char *name;
	unsigned char *HSV, *RGB;
};

struct seg_data {
	int width, height, nrgn;
	char *name;
	unsigned char *mask;
	unsigned char *HSV;
};

struct extract_data {
	cass_dataset_t ds;
	char *name;
};

struct vec_query_data {
	char *name;
	cass_dataset_t *ds;
	cass_result_t result;
};

struct rank_data {
	char *name;
	cass_dataset_t *ds;
	cass_result_t result;
};

int main( int argc, char *argv[] ) {
// Setup 
	tbb::task_scheduler_init init( tbb::task_scheduler_init::deferred );

	stimer_t tmr;

	int ret, i;

	int cnt_enqueue = 0;
	int cnt_dequeue = 0;

#ifdef PARSEC_VERSION
#define __PARSEC_STRING(x) #x
#define __PARSEC_XSTRING(x) __PARSEC_STRING(x)
        printf("PARSEC Benchmark Suite Version "__PARSEC_XSTRING(PARSEC_VERSION)"\n");
        fflush(NULL);
#else
        printf("PARSEC Benchmark Suite\n");
        fflush(NULL);
#endif //PARSEC_VERSION

	if (argc < 8)
	{
		printf("%s <database> <table> <query dir> <top K> <depth> <n> <out>\n", argv[0]); 
		return 0;
	}

	char *extra_params = "-L 8 - T 20";

	cass_env_t *env;
	cass_table_t *table;
	cass_table_t *query_table;

	int vec_dist_id = 0;
	int vecset_dist_id = 0;

	const char *db_dir 	= argv[1];
	const char *table_name	= argv[2];
	const char *query_dir 	= argv[3];

	int top_K 	= atoi( argv[4] );
	int num_tokens 	= atoi( argv[5] );
	int nthreads 	= atoi( argv[6] );

	init.initialize( nthreads );

	const char *output_path = argv[7];

	FILE *fout = fopen( output_path, "w" );
	assert( fout != NULL );

	cass_init();

	if( ( ret = cass_env_open( &env, db_dir, 0 ) ) != 0 ) {
		printf( "ERROR: %s\n", cass_strerror( ret ) ); 
		return 0;
	}

	assert( ( vec_dist_id = cass_reg_lookup( &env->vec_dist, "L2_float" ) ) >= 0 );

	assert( ( vecset_dist_id = cass_reg_lookup( &env->vecset_dist, "emd" ) ) >= 0 );

	i = cass_reg_lookup( &env->table, table_name );
	table = query_table = cass_reg_get( &env->table, i );

	if( ( i = table->parent_id ) >= 0 )
		query_table = cass_reg_get( &env->table, i );
	
	if( query_table != table )
		cass_table_load( query_table );

	cass_map_load( query_table->map );
	cass_table_load( table );

	image_init( argv[0] );

	stimer_tick( &tmr );

// Create pipeline stages
	char path[ BUFSIZ ];
	int first_call = 1;

	struct stack_data {
		DIR *dir;
		char *head;
	};
	std::stack<stack_data*> stack;

	// forward declarations
	std::function< void( const char *dir, char *head ) > enter_directory;
	std::function< void() > leave_directory;
	std::function< void( char *head ) > reset_path_head;
	std::function< load_data*( const char *file, char *head ) > load_file;
	std::function< load_data*( const char *dir, char *head ) > dispatch;
	std::function< load_data*() > read_next_item;

	// enter a directory
	enter_directory = [&]( const char *dir, char *head ) -> void {
		// open the directory
		DIR *pd = NULL;
		assert( ( pd = opendir( dir ) ) != NULL );
		// store the data
		struct stack_data *data = new stack_data;
		data->dir = pd;
		data->head = head;
		// push onto the stack
		stack.push( data );
		return;
	};

	// leave the directory (now that we've completed it)
	leave_directory = [&]() -> void {
		// remove the top of the stack
		struct stack_data *data = stack.top();
		stack.pop();
		// close the directory file descriptor
		DIR *pd = data->dir;
		if( pd != NULL )
			closedir( pd );
		// remove the appended file name from the path
		reset_path_head( data->head );
		delete data;
		return;
	};

	// remove the appended part of the path
	reset_path_head = [&]( char *head ) -> void {
		head[0] = 0;
	};

	load_file = [&]( const char *file, char *head ) -> load_data* {
		// initialize the new token
		struct load_data *data = new load_data;
		// copy the file name to the token
		data->name = strdup( file );
		// read in the image - assert checks for errors
		assert( image_read_rgb_hsv( file, &data->width, &data->height, &data->RGB, &data->HSV ) == 0 );
		// reset the path buffer head
		reset_path_head( head );
		// increment enqueued counter
		cnt_enqueue++;
		// return a pointer to the data 
		return data;
	};

	// decide what to do with a file
	dispatch = [&]( const char *dir, char *head ) -> load_data* {
		// if one of the special directories, skip
		if( dir[0] == '.' && ( dir[1] == 0 || ( dir[1] == '.' && dir[2] == 0 ) ) )
			return read_next_item(); // i.e. recurse to the next item

		struct stat st;
		// append the file name to the path
		strcat( head, dir );
		assert( stat( path, &st ) == 0 );
		if( S_ISREG( st.st_mode ) )
			// just return the loaded file
			return load_file( path, head );
		else if( S_ISDIR( st.st_mode ) ) {
			// append the path separator
			strcat( head, "/" );
			// enter the directory
			enter_directory( path, head + strlen( head ) );
			// recurse for the next item
			return read_next_item();
		}
	};

	// find the next item
	read_next_item = [&]() -> load_data* {
		// if the stack is empty, we are done
		if( stack.empty() ) 
			return NULL;
		// examine the top of the stack (current directory)
		struct dirent *ent = NULL;
		struct stack_data *data = stack.top();
		// get the next file
		ent = readdir( data->dir );
		if( ent == NULL ) {
			// we've finished the directory! close it and recurse back up the tree
			data = NULL;
			leave_directory();
			return read_next_item();
		} else {
			// figure out what to do with this file
			return dispatch( ent->d_name, data->head );
		}
	};

	auto read_f = [&]( tbb::flow_control& fc ) -> load_data* {
		struct load_data *next = NULL;
		if( first_call ) {
			// special handling at the start
			first_call = 0;
			path[0] = 0; // empty the path buffer
			if( ! strcmp( query_dir, "." ) ) {
				// if they used the special directory notation,
				// make sure to enter the current directory before getting an item
				enter_directory( ".", path );
				next = read_next_item();
				//return read_next_item();
			} else {
				// otherwise, we can just figure out what to do with the path provided
				next = dispatch( query_dir, path );
				//return dispatch( query_dir, path );
			}
		} else {
			// otherwise, just get the next item
			next = read_next_item();
			//return read_next_item();
		}
		if( next == NULL )
			fc.stop();
		return next;
	};

	auto seg_f = [&]( load_data *load ) -> seg_data* {
			struct seg_data *seg;

			seg	= new seg_data;

			seg->name 	= load->name;
			seg->width 	= load->width;
			seg->height 	= load->height;
			seg->HSV	= load->HSV;
			
			image_segment( &seg->mask, &seg->nrgn, load->RGB, load->width, load->height );

			delete load->RGB;
			delete load;

			return seg;
		};

	auto extract_f = [&]( seg_data *seg ) -> extract_data* {
				struct extract_data *extract;

				assert( seg != NULL );
				extract	= new extract_data;
				extract->name = seg->name;

				image_extract_helper( seg->HSV, seg->mask, seg->width, seg->height, seg->nrgn, &extract->ds );

				delete seg->mask;
				delete seg->HSV;
				delete seg;

				return extract;
			};

	auto query_f = [&]( extract_data *extract ) -> vec_query_data* {
			struct vec_query_data *vec;
			cass_query_t query;

			assert( extract != NULL );
			vec 		= new vec_query_data;
			vec->name 	= extract->name;

			memset( &query, 0, sizeof query );
			query.flags 		= CASS_RESULT_LISTS | CASS_RESULT_USERMEM;
			vec->ds = query.dataset = &extract->ds;
			query.vecset_id 	= 0;
			query.vec_dist_id 	= vec_dist_id;
			query.vecset_dist_id	= vecset_dist_id;
			query.topk 		= 2 * top_K;
			query.extra_params 	= extra_params;

			cass_result_alloc_list( &vec->result, vec->ds->vecset[0].num_regions, query.topk );

			cass_table_query( table, &query, &vec->result );

			return vec;
		};

	auto rank_f = [&]( vec_query_data *vec ) -> rank_data* {
			struct rank_data *rank;
			cass_result_t *candidate;
			cass_query_t query;

			assert( vec != NULL );
			rank 		= new rank_data;
			rank->name 	= vec->name;

			query.flags 		= CASS_RESULT_LIST | CASS_RESULT_USERMEM | CASS_RESULT_SORT;
			query.dataset 		= vec->ds;
			query.vecset_id 	= 0;
			query.vec_dist_id 	= vec_dist_id;
			query.vecset_dist_id 	= vecset_dist_id;
			query.topk 		= top_K;
			query.extra_params 	= NULL;

			candidate = cass_result_merge_lists( &vec->result, (cass_dataset_t*) query_table->__private, 0 );
			query.candidate = candidate;

			cass_result_alloc_list( &rank->result, 0, top_K );
			cass_table_query( query_table, &query, &rank->result );

			cass_result_free( &vec->result );
			cass_result_free( candidate );
			free( candidate );
			cass_dataset_release( vec->ds );
			free( vec->ds );
			delete vec;

			return rank;
		};

	auto write_f = [&]( rank_data *rank ) -> void {
			assert( rank != NULL );
			fprintf( fout, "%s", rank->name );

			ARRAY_BEGIN_FOREACH(rank->result.u.list, cass_list_entry_t p)
			{
				char *obj = NULL;
				if (p.dist == HUGE) continue;
				cass_map_id_to_dataobj(query_table->map, p.id, &obj);
				assert(obj != NULL);
				fprintf(fout, "\t%s:%g", obj, p.dist);
			} ARRAY_END_FOREACH;

			fprintf( fout, "\n" );

			cass_result_free( &rank->result );
			delete rank->name;
			delete rank;

			// add the count printer
			cnt_dequeue++;
			fprintf( stderr, "(%d,%d)\n", cnt_enqueue, cnt_dequeue );

			return;
		};

// Run pipeline
	tbb::parallel_pipeline( PIPELINE_WIDTH,
				tbb::make_filter< void, load_data* >( tbb::filter::serial_in_order, read_f ) &
				tbb::make_filter< load_data*, seg_data* >( tbb::filter::parallel, seg_f ) &
				tbb::make_filter< seg_data*, extract_data* >( tbb::filter::parallel, extract_f ) &
				tbb::make_filter< extract_data*, vec_query_data* >( tbb::filter::parallel, query_f ) &
				tbb::make_filter< vec_query_data*, rank_data* >( tbb::filter::parallel, rank_f ) &
				tbb::make_filter< rank_data*, void >( tbb::filter::serial_out_of_order, write_f ) );

// Clean up 
	assert( cnt_enqueue == cnt_dequeue );

	stimer_tuck( &tmr, "QUERY TIME" );

	if( ( ret = cass_env_close( env, 0 ) ) != 0 ) {
		printf( "ERROR: %s\n", cass_strerror( ret ) );
		return 0;
	}

	cass_cleanup();

	image_cleanup();

	fclose( fout );

	return 0;
}
