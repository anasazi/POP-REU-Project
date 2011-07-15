#include "tbb/task_scheduler_init.h"
#include "tbb/pipeline.h"

#define PIPELINE_WIDTH 1 // TODO pick better value

class Read : tbb::filter {
	void* operator()( void* vitem ) {
	};
};

struct load_data {
	int width, height;
	char *name;
	unsigned char *HSV, *RGB;
};

class SegmentImage : tbb::filter {
	void* operator()( void* vitem ) {
		struct load_data *load;
		struct seg_data *seg;

		load	= (load_data*) vitem;
		seg	= new seg_data;

		seg->name 	= load->name;
		seg->width 	= load->width;
		seg->height 	= load->height;
		seg->HSV	= load->HSV;
		
		image_segment( &seg->mask, &seg->nrgn, load->RGB, load->width, load->height );

		//delete[]	load->RGB;
		free( load->RGB );
		//delete 		load;
		free( load );

		return (void*) seg;
	};
};

struct seg_data {
	int width, height, nrgn;
	char *name;
	unsigned char *mask;
	unsigned char *HSV;
};

class ExtractFeatures : tbb::filter {
	void* operator()( void* vitem ) {
		struct seg_data *seg;
		struct extract_data *extract;

		seg = (seg_data*) vitem;
		assert( seg != NULL );
		extract	= new extract_data;

		image_extract_helper( seg->HSV, seg->mask, seg->width, seg->height, seg->nrgn, &extract->ds );

		//delete[]	seg->mask;
		//delete[] 	seg->HSV;
		free( seg->mask );
		free( seg->HSV );
		delete 		seg;

		return (void*) extract;
	};
};

struct extract data {
	cass_dataset_t ds;
	char *name;
};

class QueryIndex : tbb::filter {
	void* operator()( void* vitem ) {
		struct extract_data *extract;
		struct vec_query_data *vec;
		cass_query_t query;

		extract		= (extract_data*) vitem;
		assert( extract != NULL );
		vec 		= new vec_query_data;
		vec->name 	= extract->name;

		memset( &query, 0, sizeof query );
		query.flags 		= CASS_RESULT_LISTS | CASS_RESULT_USERMEM;
		vec->ds = query.dataset = &extract->ds;
		query.vecset_id 	= 0;
		query.vec_dist_id 	= vec_dist_id; // TODO pass in
		query.vecset_dist_id	= vecset_dist_id; // TODO pass in
		query.topk 		= 2 * top_K; // TODO pass in
		query.extra_params 	= extra_params; // TODO pass in

		cass_result_alloc_list( &vec->result, vec->ds->vecset[0].num_regions, query.topk );

		cass_table_query( table, &query, &vec->result ); // TODO pass in

		delete extract; // TODO is this ok?

		return (void*) vec;
	};
};

struct vec_query_data {
	char *name;
	cass_dataset_t *ds;
	cass_result_t result;
};

class RankCandidates : tbb::filter {
	void* operator()( void* vitem ) {
		struct vec_query_data *vec;
		struct rank_data *rank;
		cass_result_t *candidate;
		cass_query_t query;

		vec 		= (vec_query_data*) vitem;
		assert( vec != NULL );
		rank 		= new rank_data;
		rank->name 	= vec->name;

		query.flags 		= CASS_RESULT_LISTS | CASS_RESULT_USERMEM | CASS_RESULT_SORT;
		query.dataset 		= vec->ds;
		query.vecset_id 	= 0;
		query.vec_dist_id 	= vec_dist_id; // TODO pass in
		query.vecset_dist_id 	= vecset_dist_id; // TODO pass in
		query.topk 		= top_K; // TODO pass in
		query.extra_params 	= NULL;

		candidate = cass_result_merge_lists( &vec->result, (cass_dataset_t*) query_table->__private, 0 ); // TODO pass in
		query.candidate = candidate;

		cass_result_alloc_list( &rank->result, 0, top_K ); // TODO pass in
		cass_table_query( query_table, &query, &rank->result );

		cass_result_free( &vec->result );
		cass_result_free( candidate );
		free( candidate );
		cass_dataset_release( vec->ds );
		free( vec->ds );
		delete vec;

		return (void*) rank;
	};
};

struct rank_data {
	char *name;
	cass_dataset_t *ds;
	cass_result_t result;
};

class Write : tbb::filter {
	void* operator()( void* vitem ) {
		struct rank_data *rank;

		rank = (rank_data*) vitem;
		assert( rank != NULL );

		fprintf( fout, "%s", rank->name ); // TODO pass in

		ARRAY_BEGIN_FOREACH(rank->result.u.list, cass_list_entry_t p)
		{
			char *obj = NULL;
			if (p.dist == HUGE) continue;
			cass_map_id_to_dataobj(query_table->map, p.id, &obj); // TODO pass in
			assert(obj != NULL);
			fprintf(fout, "\t%s:%g", obj, p.dist);
		} ARRAY_END_FOREACH;

		fprintf( fout, "\n" );

		cass_result_free( &rank->result );
		delete[] rank->name;
		delete rank;

		// TODO add the count printer

		return NULL;
	};
};

int main( int argc, char *argv[] ) {
// Setup TODO 

// Create pipeline stages
// TODO fill in constructors
	Read 		reader;
	SegmentImage 	seg;
	ExtractFeatures	ext;
	QueryIndex	query;
	RankCandidates	rank;
	Write		write;
	tbb::pipeline 	pipe;

	pipe.add_filter( reader );
	pipe.add_filter( seg );
	pipe.add_filter( ext );
	pipe.add_filter( query );
	pipe.add_filter( rank );
	pipe.add_filter( write );

// Run pipeline
	pipe.run( PIPELINE_WIDTH );

// Clean up TODO
}
