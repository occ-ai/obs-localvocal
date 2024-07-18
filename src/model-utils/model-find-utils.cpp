#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <regex>

#include <obs-module.h>

#include "model-find-utils.h"
#include "plugin-support.h"

std::string find_file_in_folder_by_name(const std::string &folder_path,
					const std::string &file_name)
{
	for (const auto &entry : std::filesystem::directory_iterator(folder_path)) {
		if (entry.path().filename() == file_name) {
			return entry.path().string();
		}
	}
	return "";
}

// Find a file in a folder by expression
std::string find_file_in_folder_by_regex_expression(const std::string &folder_path,
						    const std::string &file_name_regex)
{
	if (!std::filesystem::exists(folder_path)) {
		obs_log(LOG_ERROR, "Folder does not exist: %s", folder_path.c_str());
		return "";
	}
	if (!std::filesystem::is_directory(folder_path)) {
		obs_log(LOG_ERROR, "Path is not a folder: %s", folder_path.c_str());
		return "";
	}
	if (file_name_regex.empty()) {
		obs_log(LOG_ERROR, "Empty file name regex");
		return "";
	}
	try {
		for (const auto &entry : std::filesystem::directory_iterator(folder_path)) {
			if (std::regex_match(entry.path().filename().string(),
					     std::regex(file_name_regex))) {
				return entry.path().string();
			}
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Error finding file in folder by regex expression: %s",
			e.what());
	}
	return "";
}

std::string find_bin_file_in_folder(const std::string &model_local_folder_path)
{
	// find .bin file in folder
	for (const auto &entry : std::filesystem::directory_iterator(model_local_folder_path)) {
		if (entry.path().extension() == ".bin") {
			const std::string bin_file_path = entry.path().string();
			obs_log(LOG_INFO, "Model bin file found in folder: %s",
				bin_file_path.c_str());
			return bin_file_path;
		}
	}
	obs_log(LOG_ERROR, "Model bin file not found in folder: %s",
		model_local_folder_path.c_str());
	return "";
}
