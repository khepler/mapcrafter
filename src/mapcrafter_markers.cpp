/*
 * Copyright 2012-2016 Moritz Hilscher
 *
 * This file is part of Mapcrafter.
 *
 * Mapcrafter is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mapcrafter is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Mapcrafter.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "accumulator.h"
#include "mapcraftercore/util.h"
#include "mapcraftercore/config/mapcrafterconfig.h"
#include "mapcraftercore/mc/world.h"
#include "mapcraftercore/mc/worldentities.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

namespace po = boost::program_options;
namespace fs = boost::filesystem;

namespace util = mapcrafter::util;
namespace config = mapcrafter::config;
namespace mc = mapcrafter::mc;

struct Marker {
	mc::BlockPos pos;
	std::string title, text;

	std::string toJSON() const {
		std::string json = "{";
		json += "\"pos\": [" + util::str(pos.x) + "," + util::str(pos.z) + "," + util::str(pos.y) + "], ";
		json += "\"title\": \"" + util::escapeJSON(title) + "\", ";
		json += "\"text\": \"" + util::escapeJSON(text) + "\", ";
		return json + "}";
	}
};

typedef std::map<std::string, std::vector<Marker>> MarkerGroup;
// map (marker group name -> map ( world name -> array of markers) )
typedef std::map<std::string, MarkerGroup > Markers;

Markers findMarkers(const config::MapcrafterConfig& config) {
	Markers markers;
	auto groups = config.getMarkers();
	for (auto group_it = groups.begin(); group_it != groups.end(); ++group_it)
		markers[group_it->getShortName()];

	auto config_worlds = config.getWorlds();
	auto config_markers = config.getMarkers();
	for (auto world_it = config_worlds.begin(); world_it != config_worlds.end();
			++world_it) {
		mc::WorldCrop world_crop = world_it->second.getWorldCrop();
		mc::World world(world_it->second.getInputDir().string(),
				world_it->second.getDimension(), config.getCachePath(world_it->second.getShortName()).string());
		world.setWorldCrop(world_crop);
		if (!world.load()) {
			LOG(ERROR) << "Unable to load world " << world_it->first << "!";
			continue;
		}

		LOGN(INFO, "progress") << "Loading entities of world '" << world_it->first << "' ...";
		mc::WorldEntitiesCache entities(world);
		util::LogOutputProgressHandler progress;
		entities.update(&progress);

		// use name of the world section as world name, not the world_name
		std::string world_name = world_it->second.getShortName();
		std::vector<mc::SignEntity> signs = entities.getSigns(world.getWorldCrop());
		for (auto sign_it = signs.begin(); sign_it != signs.end(); ++sign_it) {
			// don't use signs not contained in the world boundaries
			if (!world_crop.isBlockContainedXZ(sign_it->getPos())
					&& !world_crop.isBlockContainedY(sign_it->getPos()))
				continue;
			for (auto marker_it = config_markers.begin();
					marker_it != config_markers.end(); ++marker_it) {
				if (!marker_it->matchesSign(*sign_it))
					continue;
				Marker marker;
				marker.pos = sign_it->getPos();
				marker.title = marker_it->formatTitle(*sign_it);
				marker.text = marker_it->formatText(*sign_it);
				markers[marker_it->getShortName()][world_name].push_back(marker);
				LOG(DEBUG) << "Found marker (prefix '" << marker_it->getPrefix()
						<< "'): '" << marker.title << "' at '" << world_it->first
						<< "':" << marker.pos;
				break;
			}
		}
	}
	return markers;
}

std::string createMarkersJSON(const config::MapcrafterConfig& config,
		const Markers& markers_found) {
	auto markers = config.getMarkers();
	std::stringstream ss;

	ss << "// This file is automatically generated. Do not edit this file." << std::endl;
	ss << "// Use the markers.js for your own markers instead." << std::endl << std::endl;
	ss << "MAPCRAFTER_MARKERS_GENERATED = [" << std::endl;
	for (auto marker_config_it = markers.begin(); marker_config_it != markers.end();
			++marker_config_it) {
		config::MarkerSection marker_config = *marker_config_it;
		std::string group = marker_config.getShortName();
		ss << "  {" << std::endl;
		ss << "    \"id\" : \"" << group << "\"," << std::endl;
		ss << "    \"name\" : \"" << marker_config.getLongName() << "\"," << std::endl;
		if (!marker_config.getIcon().empty()) {
			ss << "    \"icon\" : \"" << marker_config.getIcon() << "\"," << std::endl;
			if (!marker_config.getIconSize().empty())
				ss << "    \"iconSize\" : " << marker_config.getIconSize() << "," << std::endl;
		}
		ss << "    \"showDefault\" : ";
		ss << (marker_config.isShownByDefault() ? "true" : "false") << "," << std::endl;
		ss << "    \"markers\" : {" << std::endl;

		if (!markers_found.count(group)) {
			ss << "    }," << std::endl;
			ss << "  }," << std::endl;
			continue;
		}

		for (auto world_it = markers_found.at(group).begin();
				world_it != markers_found.at(group).end(); ++world_it) {
			ss << "      \"" << world_it->first << "\" : [" << std::endl;
			for (auto marker_it = world_it->second.begin();
					marker_it != world_it->second.end(); ++marker_it) {
				ss << "        " << marker_it->toJSON() << "," << std::endl;
			}
			ss << "      ]," << std::endl;
		}
		ss << "    }," << std::endl;
		ss << "  }," << std::endl;
	}
	ss << "];" << std::endl;

	return ss.str();
}

int main(int argc, char** argv) {
	std::string config_file;
	std::string output_file;
	int verbosity = 0;

	po::options_description all("Allowed options");
	all.add_options()
		("help,h", "shows this help message")
		("verbose,v", accumulator<int>(&verbosity),
				"verbose blah blah")

		("config,c", po::value<std::string>(&config_file),
			"the path to the configuration file (required)")
		("output-file,o", po::value<std::string>(&output_file),
			"file to write the generated markers to, "
			"defaults to markers-generated.js in the output directory.");

	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, all), vm);
	} catch (po::error& ex) {
		std::cout << "There is a problem parsing the command line arguments: "
				<< ex.what() << std::endl << std::endl;
		std::cout << all << std::endl;
		return 1;
	}

	po::notify(vm);

	if (vm.count("help")) {
		std::cout << all << std::endl;
		return 1;
	}

	if (!vm.count("config")) {
		std::cerr << "You have to specify a configuration file!" << std::endl;
		return 1;
	}

	// TODO add this to an automatic logging configuration?
	util::LogLevel log_level = util::LogLevel::WARNING;
	if (verbosity == 1)
		log_level = util::LogLevel::INFO;
	else if (verbosity > 1)
		log_level = util::LogLevel::DEBUG;
	util::Logging::getInstance().setSinkVerbosity("__output__", log_level);
	util::Logging::getInstance().setSinkLogProgress("__output__", true);

	config::MapcrafterConfig config;
	config::ValidationMap validation = config.parseFile(config_file);

	if (!validation.isEmpty()) {
		if (validation.isCritical())
			LOG(FATAL) << "Your configuration file is invalid!";
		else
			LOG(WARNING) << "Some notes on your configuration file:";
		validation.log();
		LOG(WARNING) << "Please read the documentation about the new configuration file format.";
	}

	Markers markers = findMarkers(config);

	// count how many markers / markers of which group were found
	int markers_count = 0;
	std::map<std::string, int> groups_count;

	auto worlds = config.getWorlds();
	auto groups = config.getMarkers();
	for (auto group_it = groups.begin(); group_it != groups.end(); ++group_it) {
		std::string group = group_it->getShortName();
		groups_count[group] = 0;
		for (auto world_it = worlds.begin(); world_it != worlds.end(); ++world_it)
			groups_count[group] += markers[group][world_it->first].size();
		markers_count += groups_count[group];
	}

	// and log some stats about that
	LOG(INFO) << "Found " << markers_count << " markers in " << markers.size() << " categories:";
	for (auto group_it = groups.begin(); group_it != groups.end(); ++group_it) {
		std::string group = group_it->getShortName();
		LOG(INFO) << "  Markers with prefix '" << config.getMarker(group).getPrefix()
				<< "': " << groups_count[group];
		for (auto world_it = worlds.begin(); world_it != worlds.end(); ++world_it)
			LOG(INFO) << "    in world '" << world_it->first << "': "
					<< markers[group][world_it->first].size();
	}

	if (output_file == "-")
		std::cout << createMarkersJSON(config, markers);
	else {
		if (output_file == "")
			output_file = config.getOutputPath("markers-generated.js").string();
		std::ofstream out(output_file);
		out << createMarkersJSON(config, markers);
		out.close();
		if (!out) {
			LOG(ERROR) << "Unable to write to file '" << output_file << "'!";
			return 1;
		}
	}
}
