
#include "plugin-support.h"
#include "timed-metadata-utils.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <vector>

#include <sstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>

#include <curl/curl.h>

#include <obs-module.h>

#include <nlohmann/json.hpp>

// HMAC SHA-256 function
std::string hmacSha256(const std::string &key, const std::string &data, bool isHexKey = false)
{
	unsigned char *digest;
	size_t len = EVP_MAX_MD_SIZE;
	digest = (unsigned char *)bzalloc(len);

	EVP_PKEY *pkey = nullptr;
	if (isHexKey) {
		// Convert hex string to binary data
		std::vector<unsigned char> hexKey;
		for (size_t i = 0; i < key.length(); i += 2) {
			std::string byteString = key.substr(i, 2);
			unsigned char byte = (unsigned char)strtol(byteString.c_str(), NULL, 16);
			hexKey.push_back(byte);
		}
		pkey = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, hexKey.data(), (int)hexKey.size());
	} else {
		pkey = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, (unsigned char *)key.c_str(),
					    (int)key.length());
	}

	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey);
	EVP_DigestSignUpdate(ctx, data.c_str(), data.length());
	EVP_DigestSignFinal(ctx, digest, &len);

	EVP_PKEY_free(pkey);
	EVP_MD_CTX_free(ctx);

	std::stringstream ss;
	for (size_t i = 0; i < len; ++i) {
		ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
	}
	bfree(digest);
	return ss.str();
}

std::string sha256(const std::string &data)
{
	unsigned char hash[EVP_MAX_MD_SIZE];
	unsigned int lengthOfHash = 0;

	EVP_MD_CTX *context = EVP_MD_CTX_new();

	if (context != nullptr) {
		if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr)) {
			if (EVP_DigestUpdate(context, data.c_str(), data.length())) {
				if (EVP_DigestFinal_ex(context, hash, &lengthOfHash)) {
					EVP_MD_CTX_free(context);

					std::stringstream ss;
					for (unsigned int i = 0; i < lengthOfHash; ++i) {
						ss << std::hex << std::setw(2) << std::setfill('0')
						   << (int)hash[i];
					}
					return ss.str();
				}
			}
		}
		EVP_MD_CTX_free(context);
	}

	return "";
}

std::string getCurrentTimestamp()
{
	auto now = std::chrono::system_clock::now();
	auto in_time_t = std::chrono::system_clock::to_time_t(now);
	std::stringstream ss;
	ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%dT%H%M%SZ");
	return ss.str();
}

std::string getCurrentDate()
{
	auto now = std::chrono::system_clock::now();
	auto in_time_t = std::chrono::system_clock::to_time_t(now);
	std::stringstream ss;
	ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%d");
	return ss.str();
}

size_t WriteCallback(void *ptr, size_t size, size_t nmemb, std::string *data)
{
	data->append((char *)ptr, size * nmemb);
	return size * nmemb;
}

void send_timed_metadata_to_ivs_endpoint(struct transcription_filter_data *gf,
					 Translation_Mode mode, const std::string &source_text,
					 const std::string &source_lang,
					 const std::string &target_text,
					 const std::string &target_lang)
{
	// below 4 should be from a configuration
	std::string AWS_ACCESS_KEY = gf->aws_access_key;
	std::string AWS_SECRET_KEY = gf->aws_secret_key;
	std::string CHANNEL_ARN = gf->ivs_channel_arn;
	std::string REGION = gf->aws_region;

	std::string SERVICE = "ivs";
	std::string HOST = "ivs." + REGION + ".amazonaws.com";

	// Construct the inner JSON string
	nlohmann::json inner_meta_data;
	if (mode == NON_WHISPER_TRANSLATE) {
		obs_log(gf->log_level,
			"send_timed_metadata_to_ivs_endpoint - NON_WHISPER_TRANSLATE");
		nlohmann::json array;
		if (!source_text.empty()) {
			array.push_back({{"language", source_lang}, {"text", source_text}});
		}
		if (!target_text.empty()) {
			array.push_back({{"language", target_lang}, {"text", target_text}});
		}
		if (array.empty()) {
			obs_log(gf->log_level,
				"send_timed_metadata_to_ivs_endpoint - source and target text empty");
			return;
		}
		inner_meta_data = {{"captions", array}};
	} else if (mode == WHISPER_TRANSLATE) {
		if (target_text.empty()) {
			obs_log(gf->log_level,
				"send_timed_metadata_to_ivs_endpoint - target text empty");
			return;
		}
		obs_log(gf->log_level, "send_timed_metadata_to_ivs_endpoint - WHISPER_TRANSLATE");
		inner_meta_data = {
			{"captions", {{{"language", target_lang}, {"text", target_text}}}}};
	} else {
		if (source_text.empty()) {
			obs_log(gf->log_level,
				"send_timed_metadata_to_ivs_endpoint - source text empty");
			return;
		}
		obs_log(gf->log_level, "send_timed_metadata_to_ivs_endpoint - transcription mode");
		inner_meta_data = {
			{"captions", {{{"language", source_lang}, {"text", source_text}}}}};
	}

	// Construct the outer JSON string
	nlohmann::json inner_meta_data_as_string = inner_meta_data.dump();
	std::string METADATA = R"({
        "channelArn": ")" + CHANNEL_ARN +
			       R"(",
        "metadata": )" + inner_meta_data_as_string.dump() +
			       R"(
    })";

	std::string DATE = getCurrentDate();
	std::string TIMESTAMP = getCurrentTimestamp();
	std::string PAYLOAD_HASH = sha256(METADATA);

	std::ostringstream canonicalRequest;
	canonicalRequest << "POST\n"
			 << "/PutMetadata\n"
			 << "\n"
			 << "content-type:application/json\n"
			 << "host:" << HOST << "\n"
			 << "x-amz-date:" << TIMESTAMP << "\n"
			 << "\n"
			 << "content-type;host;x-amz-date\n"
			 << PAYLOAD_HASH;
	std::string CANONICAL_REQUEST = canonicalRequest.str();
	std::string HASHED_CANONICAL_REQUEST = sha256(CANONICAL_REQUEST);

	std::string ALGORITHM = "AWS4-HMAC-SHA256";
	std::string CREDENTIAL_SCOPE = DATE + "/" + REGION + "/" + SERVICE + "/aws4_request";
	std::ostringstream stringToSign;
	stringToSign << ALGORITHM << "\n"
		     << TIMESTAMP << "\n"
		     << CREDENTIAL_SCOPE << "\n"
		     << HASHED_CANONICAL_REQUEST;
	std::string STRING_TO_SIGN = stringToSign.str();

	std::string KEY = "AWS4" + AWS_SECRET_KEY;
	std::string DATE_KEY = hmacSha256(KEY, DATE);
	std::string REGION_KEY = hmacSha256(DATE_KEY, REGION, true);
	std::string SERVICE_KEY = hmacSha256(REGION_KEY, SERVICE, true);
	std::string SIGNING_KEY = hmacSha256(SERVICE_KEY, "aws4_request", true);
	std::string SIGNATURE = hmacSha256(SIGNING_KEY, STRING_TO_SIGN, true);

	std::ostringstream authHeader;
	authHeader << ALGORITHM << " Credential=" << AWS_ACCESS_KEY << "/" << CREDENTIAL_SCOPE
		   << ", SignedHeaders=content-type;host;x-amz-date, Signature=" << SIGNATURE;

	std::string AUTH_HEADER = authHeader.str();

	// Initialize CURL and set options
	CURL *curl;
	CURLcode res;
	curl = curl_easy_init();
	if (!curl) {
		obs_log(LOG_ERROR,
			"send_timed_metadata_to_ivs_endpoint failed: curl_easy_init failed");
		return;
	}

	curl_easy_setopt(curl, CURLOPT_URL, ("https://" + HOST + "/PutMetadata").c_str());
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, ("Host: " + HOST).c_str());
	headers = curl_slist_append(headers, ("x-amz-date: " + TIMESTAMP).c_str());
	headers = curl_slist_append(headers, ("Authorization: " + AUTH_HEADER).c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, METADATA.c_str());

	std::string response_string;
	std::string header_string;
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_string);

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		obs_log(LOG_WARNING, "send_timed_metadata_to_ivs_endpoint failed:%s",
			curl_easy_strerror(res));
	} else {
		long response_code;
		// Get the HTTP response code
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		obs_log(gf->log_level, "HTTP Status code: %ld", response_code);
		if (response_code != 204) {
			obs_log(LOG_WARNING, "HTTP response: %s", response_string.c_str());
		}
	}
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
}

// source: transcription text, target: translation text
void send_timed_metadata_to_server(struct transcription_filter_data *gf, Translation_Mode mode,
				   const std::string &source_text, const std::string &source_lang,
				   const std::string &target_text, const std::string &target_lang)
{
	if (gf->aws_access_key.empty() || gf->aws_secret_key.empty() ||
	    gf->ivs_channel_arn.empty() || gf->aws_region.empty()) {
		obs_log(gf->log_level,
			"send_timed_metadata_to_server failed: IVS settings not set");
		return;
	}

	std::thread send_timed_metadata_thread([=]() {
		send_timed_metadata_to_ivs_endpoint(gf, mode, source_text, source_lang, target_text,
						    target_lang);
	});
	send_timed_metadata_thread.detach();
}
