#ifndef CODE_MAP_DATA_H
#define CODE_MAP_DATA_H

#include <map>
#include <utility>
#include <vector>

#include "FilePath.h"
#include "types.h"

// File level summary of the whole index: every indexed file and how often one file uses another.
struct CodeMapData
{
	struct File
	{
		Id nodeId = 0;
		FilePath path;
		size_t symbolCount = 0;
	};

	std::vector<File> files;
	std::map<std::pair<Id, Id>, size_t> dependencies;
};

#endif	  // CODE_MAP_DATA_H
