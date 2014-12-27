#include <iostream>
#include <iomanip>
#include <mutex>
#include <atomic>

//#define DBMS_HASH_MAP_DEBUG_RESIZES

#include <DB/Interpreters/AggregationCommon.h>

#include <DB/Common/HashTable/HashMap.h>
#include <DB/Common/HashTable/TwoLevelHashTable.h>
//#include <DB/Common/HashTable/HashTableWithSmallLocks.h>
//#include <DB/Common/HashTable/HashTableMerge.h>

#include <DB/IO/ReadBufferFromFile.h>
#include <DB/IO/CompressedReadBuffer.h>

#include <statdaemons/Stopwatch.h>
#include <statdaemons/threadpool.hpp>


typedef UInt64 Key;
typedef UInt64 Value;
typedef std::vector<Key> Source;


template <typename Map>
struct AggregateIndependent
{
	template <typename Creator, typename Updater>
	static void NO_INLINE execute(const Source & data, size_t num_threads, std::vector<std::unique_ptr<Map>> & results,
						Creator && creator, Updater && updater,
						boost::threadpool::pool & pool)
	{
		results.reserve(num_threads);
		for (size_t i = 0; i < num_threads; ++i)
			results.emplace_back(new Map);

		for (size_t i = 0; i < num_threads; ++i)
		{
			auto begin = data.begin() + (data.size() * i) / num_threads;
			auto end = data.begin() + (data.size() * (i + 1)) / num_threads;
			auto & map = *results[i];

			pool.schedule([&, begin, end]()
			{
				for (auto it = begin; it != end; ++it)
				{
					typename Map::iterator place;
					bool inserted;
					map.emplace(*it, place, inserted);

					if (inserted)
						creator(place->second);
					else
						updater(place->second);
				}
			});
		}

		pool.wait();
	}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

template <typename Map>
struct AggregateIndependentWithSequentialKeysOptimization
{
	template <typename Creator, typename Updater>
	static void NO_INLINE execute(const Source & data, size_t num_threads, std::vector<std::unique_ptr<Map>> & results,
						Creator && creator, Updater && updater,
						boost::threadpool::pool & pool)
	{
		results.reserve(num_threads);
		for (size_t i = 0; i < num_threads; ++i)
			results.emplace_back(new Map);

		for (size_t i = 0; i < num_threads; ++i)
		{
			auto begin = data.begin() + (data.size() * i) / num_threads;
			auto end = data.begin() + (data.size() * (i + 1)) / num_threads;
			auto & map = *results[i];

			pool.schedule([&, begin, end]()
			{
				typename Map::iterator place;
				Key prev_key {};
				for (auto it = begin; it != end; ++it)
				{
					if (it != begin && *it == prev_key)
					{
						updater(place->second);
						continue;
					}
					prev_key = *it;

					bool inserted;
					map.emplace(*it, place, inserted);

					if (inserted)
						creator(place->second);
					else
						updater(place->second);
				}
			});
		}

		pool.wait();
	}
};

#pragma GCC diagnostic pop


template <typename Map>
struct MergeSequential
{
	template <typename Merger>
	static void NO_INLINE execute(Map ** source_maps, size_t num_maps, Map *& result_map,
						Merger && merger,
						boost::threadpool::pool & pool)
	{
		for (size_t i = 1; i < num_maps; ++i)
		{
			auto begin = source_maps[i]->begin();
			auto end = source_maps[i]->end();
			for (auto it = begin; it != end; ++it)
				merger((*source_maps[0])[it->first], it->second);
		}

		result_map = source_maps[0];
	}
};

template <typename Map>
struct MergeSequentialTransposed	/// На практике не лучше обычного.
{
	template <typename Merger>
	static void NO_INLINE execute(Map ** source_maps, size_t num_maps, Map *& result_map,
						Merger && merger,
						boost::threadpool::pool & pool)
	{
		typename Map::iterator iterators[num_maps];
		for (size_t i = 1; i < num_maps; ++i)
			iterators[i] = source_maps[i]->begin();

		result_map = source_maps[0];

		while (true)
		{
			bool finish = true;
			for (size_t i = 1; i < num_maps; ++i)
			{
				if (iterators[i] == source_maps[i]->end())
					continue;

				finish = false;
				merger((*result_map)[iterators[i]->first], iterators[i]->second);
				++iterators[i];
			}

			if (finish)
				break;
		}
	}
};

template <typename Map, typename ImplMerge>
struct MergeParallelForTwoLevelTable
{
	template <typename Merger>
	static void NO_INLINE execute(Map ** source_maps, size_t num_maps, Map *& result_map,
						Merger && merger,
						boost::threadpool::pool & pool)
	{
		for (size_t bucket = 0; bucket < Map::NUM_BUCKETS; ++bucket)
			pool.schedule([&, bucket, num_maps]
			{
				std::vector<typename Map::Impl *> section(num_maps);
				for (size_t i = 0; i < num_maps; ++i)
					section[i] = &source_maps[i]->impls[bucket];

				typename Map::Impl * result_map;
				ImplMerge::execute(section.data(), num_maps, result_map, merger, pool);
			});

		pool.wait();
		result_map = source_maps[0];
	}
};


template <typename Map, typename Aggregate, typename Merge>
struct Work
{
	template <typename Creator, typename Updater, typename Merger>
	static void NO_INLINE execute(const Source & data, size_t num_threads,
						Creator && creator, Updater && updater, Merger && merger,
						boost::threadpool::pool & pool)
	{
		std::vector<std::unique_ptr<Map>> intermediate_results;

		Stopwatch watch;

		Aggregate::execute(data, num_threads, intermediate_results, std::forward<Creator>(creator), std::forward<Updater>(updater), pool);
		size_t num_maps = intermediate_results.size();

		watch.stop();
		double time_aggregated = watch.elapsedSeconds();
		std::cerr
			<< "Aggregated in " << time_aggregated
			<< " (" << data.size() / time_aggregated << " elem/sec.)"
			<< std::endl;

		size_t size_before_merge = 0;
		std::cerr << "Sizes: ";
		for (size_t i = 0; i < num_threads; ++i)
		{
			std::cerr << (i == 0 ? "" : ", ") << intermediate_results[i]->size();
			size_before_merge += intermediate_results[i]->size();
		}
		std::cerr << std::endl;

		watch.restart();

		std::vector<Map*> intermediate_results_ptrs(num_maps);
		for (size_t i = 0; i < num_maps; ++i)
			intermediate_results_ptrs[i] = intermediate_results[i].get();

		Map * result_map;
		Merge::execute(intermediate_results_ptrs.data(), num_maps, result_map, std::forward<Merger>(merger), pool);

		watch.stop();
		double time_merged = watch.elapsedSeconds();
		std::cerr
			<< "Merged in " << time_merged
			<< " (" << size_before_merge / time_merged << " elem/sec.)"
			<< std::endl;

		double time_total = time_aggregated + time_merged;
		std::cerr
			<< "Total in " << time_total
			<< " (" << data.size() / time_total << " elem/sec.)"
			<< std::endl;
		std::cerr << "Size: " << result_map->size() << std::endl << std::endl;
	}
};


typedef HashMap<Key, Value, HashCRC32<Key>> Map;
typedef TwoLevelHashMap<Key, Value, HashCRC32<Key>> MapTwoLevel;
typedef Poco::FastMutex Mutex;


struct Creator
{
	void operator()(Value & x) const {}
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

struct Updater
{
	void operator()(Value & x) const { ++x; }
};

#pragma GCC diagnostic pop

struct Merger
{
	void operator()(Value & dst, const Value & src) const { dst += src; }
};



int main(int argc, char ** argv)
{
	size_t n = atoi(argv[1]);
	size_t num_threads = atoi(argv[2]);
	size_t method = argc <= 3 ? 0 : atoi(argv[3]);

	std::cerr << std::fixed << std::setprecision(2);

	boost::threadpool::pool pool(num_threads);

	Source data(n);

	{
		Stopwatch watch;
		DB::ReadBufferFromFileDescriptor in1(STDIN_FILENO);
		DB::CompressedReadBuffer in2(in1);

		in2.readStrict(reinterpret_cast<char*>(&data[0]), sizeof(data[0]) * n);

		watch.stop();
		std::cerr << std::fixed << std::setprecision(2)
			<< "Vector. Size: " << n
			<< ", elapsed: " << watch.elapsedSeconds()
			<< " (" << n / watch.elapsedSeconds() << " elem/sec.)"
			<< std::endl << std::endl;
	}

	Creator creator;
	Updater updater;
	Merger merger;

	if (!method || method == 1)
		Work<
			Map,
			AggregateIndependent<Map>,
			MergeSequential<Map>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 2)
		Work<
			Map,
			AggregateIndependentWithSequentialKeysOptimization<Map>,
			MergeSequential<Map>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 3)
		Work<
			Map,
			AggregateIndependent<Map>,
			MergeSequentialTransposed<Map>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 4)
		Work<
			Map,
			AggregateIndependentWithSequentialKeysOptimization<Map>,
			MergeSequentialTransposed<Map>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 5)
		Work<
			MapTwoLevel,
			AggregateIndependent<MapTwoLevel>,
			MergeSequential<MapTwoLevel>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 6)
		Work<
			MapTwoLevel,
			AggregateIndependentWithSequentialKeysOptimization<MapTwoLevel>,
			MergeSequential<MapTwoLevel>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 7)
		Work<
			MapTwoLevel,
			AggregateIndependent<MapTwoLevel>,
			MergeSequentialTransposed<MapTwoLevel>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 8)
		Work<
			MapTwoLevel,
			AggregateIndependentWithSequentialKeysOptimization<MapTwoLevel>,
			MergeSequentialTransposed<MapTwoLevel>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 9)
		Work<
			MapTwoLevel,
			AggregateIndependent<MapTwoLevel>,
			MergeParallelForTwoLevelTable<MapTwoLevel, MergeSequential<MapTwoLevel::Impl>>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 10)
		Work<
			MapTwoLevel,
			AggregateIndependentWithSequentialKeysOptimization<MapTwoLevel>,
			MergeParallelForTwoLevelTable<MapTwoLevel, MergeSequential<MapTwoLevel::Impl>>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 13)
		Work<
			MapTwoLevel,
			AggregateIndependent<MapTwoLevel>,
			MergeParallelForTwoLevelTable<MapTwoLevel, MergeSequentialTransposed<MapTwoLevel::Impl>>
		>::execute(data, num_threads, creator, updater, merger, pool);

	if (!method || method == 14)
		Work<
			MapTwoLevel,
			AggregateIndependentWithSequentialKeysOptimization<MapTwoLevel>,
			MergeParallelForTwoLevelTable<MapTwoLevel, MergeSequentialTransposed<MapTwoLevel::Impl>>
		>::execute(data, num_threads, creator, updater, merger, pool);

	return 0;
}
