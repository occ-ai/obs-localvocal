#include <ctranslate2/translation.h>
#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

#include "token-buffer-thread.h"
#include "whisper-utils.h"
#include "transcription-utils.h"

#include <obs-module.h>

#ifdef _WIN32
#include <Windows.h>
#define SPACE L" "
#define NEWLINE L"\n"
#else
#define SPACE " "
#define NEWLINE "\n"
#endif

TokenBufferThread::TokenBufferThread() noexcept
	: gf(nullptr),
	  numSentences(2),
	  numPerSentence(30),
	  maxTime(0),
	  stop(true),
	  presentationQueueMutex(),
	  inputQueueMutex(),
	  segmentation(SEGMENTATION_TOKEN)
{
}

TokenBufferThread::~TokenBufferThread()
{
	stopThread();
}

void TokenBufferThread::initialize(
	struct transcription_filter_data *gf_,
	std::function<void(const std::string &)> captionPresentationCallback_,
	std::function<void(const std::string &)> sentenceOutputCallback_, size_t numSentences_,
	size_t numPerSentence_, std::chrono::seconds maxTime_,
	TokenBufferSegmentation segmentation_)
{
	this->gf = gf_;
	this->captionPresentationCallback = captionPresentationCallback_;
	this->sentenceOutputCallback = sentenceOutputCallback_;
	this->numSentences = numSentences_;
	this->numPerSentence = numPerSentence_;
	this->segmentation = segmentation_;
	this->maxTime = maxTime_;
	this->stop = false;
	this->workerThread = std::thread(&TokenBufferThread::monitor, this);
	this->lastContributionTime = std::chrono::steady_clock::now();
	this->lastCaptionTime = std::chrono::steady_clock::now();
}

void TokenBufferThread::stopThread()
{
	{
		std::lock_guard<std::mutex> lock(presentationQueueMutex);
		stop = true;
	}
	cv.notify_all();
	if (workerThread.joinable()) {
		workerThread.join();
	}
}

void TokenBufferThread::log_token_vector(const std::vector<std::string> &tokens)
{
	std::string output;
	for (const auto &token : tokens) {
		output += token;
	}
	obs_log(LOG_INFO, "TokenBufferThread::log_token_vector: '%s'", output.c_str());
}

void TokenBufferThread::addSentence(const std::string &sentence)
{
#ifdef _WIN32
	// on windows convert from multibyte to wide char
	int count =
		MultiByteToWideChar(CP_UTF8, 0, sentence.c_str(), (int)sentence.length(), NULL, 0);
	std::wstring sentence_ws(count, 0);
	MultiByteToWideChar(CP_UTF8, 0, sentence.c_str(), (int)sentence.length(), &sentence_ws[0],
			    count);
#else
	std::string sentence_ws = sentence;
#endif
	// split to characters
	std::vector<TokenBufferString> characters;
	for (const auto &c : sentence_ws) {
		characters.push_back(TokenBufferString(1, c));
	}

	std::lock_guard<std::mutex> lock(inputQueueMutex);

	// add the characters to the inputQueue
	for (const auto &character : characters) {
		inputQueue.push_back(character);
	}
	inputQueue.push_back(SPACE);

	// add to the contribution queue as well
	for (const auto &character : characters) {
		contributionQueue.push_back(character);
	}
	contributionQueue.push_back(SPACE);
	this->lastContributionTime = std::chrono::steady_clock::now();
}

void TokenBufferThread::clear()
{
	{
		std::lock_guard<std::mutex> lock(inputQueueMutex);
		inputQueue.clear();
	}
	{
		std::lock_guard<std::mutex> lock(presentationQueueMutex);
		presentationQueue.clear();
	}
	this->lastCaption = "";
	this->lastCaptionTime = std::chrono::steady_clock::now();
	this->captionPresentationCallback("");
}

void TokenBufferThread::monitor()
{
	obs_log(LOG_INFO, "TokenBufferThread::monitor");

	this->captionPresentationCallback("");

	while (true) {
		std::string caption_out;

		{
			std::lock_guard<std::mutex> lockPresentation(presentationQueueMutex);
			if (stop) {
				break;
			}

			// condition presentation queue
			if (presentationQueue.size() == this->numSentences * this->numPerSentence) {
				// pop a whole sentence from the presentation queue front
				for (size_t i = 0; i < this->numPerSentence; i++) {
					presentationQueue.pop_front();
				}
				if (this->segmentation == SEGMENTATION_TOKEN) {
					// pop tokens until a space is found
					while (!presentationQueue.empty() &&
					       presentationQueue.front() != SPACE) {
						presentationQueue.pop_front();
					}
				}
			}

			{
				std::lock_guard<std::mutex> lock(inputQueueMutex);

				if (!inputQueue.empty()) {
					// if there are token on the input queue
					// then add to the presentation queue based on the segmentation
					if (this->segmentation == SEGMENTATION_SENTENCE) {
						// add all the tokens from the input queue to the presentation queue
						for (const auto &token : inputQueue) {
							presentationQueue.push_back(token);
						}
						inputQueue.clear();
					} else if (this->segmentation == SEGMENTATION_TOKEN) {
						// add one token to the presentation queue
						presentationQueue.push_back(inputQueue.front());
						inputQueue.pop_front();
					} else {
						// skip spaces in the beginning of the input queue
						while (!inputQueue.empty() &&
						       inputQueue.front() == SPACE) {
							inputQueue.pop_front();
						}
						// add one word to the presentation queue
						TokenBufferString word;
						while (!inputQueue.empty() &&
						       inputQueue.front() != SPACE) {
							word += inputQueue.front();
							inputQueue.pop_front();
						}
						presentationQueue.push_back(word);
					}
				}
			}

			if (presentationQueue.size() > 0) {
				// build a caption from the presentation queue in sentences
				// with a maximum of numPerSentence tokens/words per sentence
				// and a newline between sentences
				std::vector<TokenBufferString> sentences(1);

				if (this->segmentation == SEGMENTATION_WORD) {
					// add words from the presentation queue to the sentences
					// if a sentence is full - start a new one
					size_t wordsInSentence = 0;
					for (size_t i = 0; i < presentationQueue.size(); i++) {
						const auto &word = presentationQueue[i];
						sentences.back() += word + SPACE;
						wordsInSentence++;
						if (wordsInSentence == this->numPerSentence) {
							sentences.push_back(TokenBufferString());
						}
					}
				} else {
					// iterate through the presentation queue tokens and build a caption
					for (size_t i = 0; i < presentationQueue.size(); i++) {
						const auto &token = presentationQueue[i];
						// skip spaces in the beginning of a sentence (tokensInSentence == 0)
						if (token == SPACE &&
						    sentences.back().length() == 0) {
							continue;
						}

						sentences.back() += token;
						if (sentences.back().length() ==
						    this->numPerSentence) {
							// if the next character is not a space - this is a broken word
							// roll back to the last space, replace it with a newline
							size_t lastSpace =
								sentences.back().find_last_of(
									SPACE);
							sentences.push_back(sentences.back().substr(
								lastSpace + 1));
							sentences[sentences.size() - 2] =
								sentences[sentences.size() - 2]
									.substr(0, lastSpace);
						}
					}
				}

				TokenBufferString caption;
				// if there are more sentences than numSentences - remove the oldest ones
				while (sentences.size() > this->numSentences) {
					sentences.erase(sentences.begin());
				}
				// if there are less sentences than numSentences - add empty sentences
				while (sentences.size() < this->numSentences) {
					sentences.push_back(TokenBufferString());
				}
				// build the caption from the sentences
				for (const auto &sentence : sentences) {
					if (!sentence.empty()) {
						caption += trim<TokenBufferString>(sentence);
					}
					caption += NEWLINE;
				}

#ifdef _WIN32
				// convert caption to multibyte for obs
				int count = WideCharToMultiByte(CP_UTF8, 0, caption.c_str(),
								(int)caption.length(), NULL, 0,
								NULL, NULL);
				caption_out = std::string(count, 0);
				WideCharToMultiByte(CP_UTF8, 0, caption.c_str(),
						    (int)caption.length(), &caption_out[0], count,
						    NULL, NULL);
#else
				caption_out = std::string(caption.begin(), caption.end());
#endif
			}
		}

		if (this->stop) {
			break;
		}

		const auto now = std::chrono::steady_clock::now();

		// check if enough time passed since last contribution (debounce)
		const auto durationSinceLastContribution =
			std::chrono::duration_cast<std::chrono::seconds>(
				now - this->lastContributionTime);
		if (durationSinceLastContribution > std::chrono::milliseconds(500)) {
			if (!lastContributionIsSent) {
				// take the contribution queue and send it to the output
				TokenBufferString contribution;
				for (const auto &token : contributionQueue) {
					contribution += token;
				}
				contributionQueue.clear();
#ifdef _WIN32
				// convert caption to multibyte for obs
				int count = WideCharToMultiByte(CP_UTF8, 0, contribution.c_str(),
								(int)contribution.length(), NULL, 0,
								NULL, NULL);
				std::string contribution_out = std::string(count, 0);
				WideCharToMultiByte(CP_UTF8, 0, contribution.c_str(),
						    (int)contribution.length(),
						    &contribution_out[0], count, NULL, NULL);
#else
				std::string contribution_out(contribution.begin(),
							     contribution.end());
#endif

				obs_log(LOG_INFO, "TokenBufferThread::monitor: output '%s'",
					contribution_out.c_str());
				this->sentenceOutputCallback(contribution_out);
				lastContributionIsSent = true;
			}
		} else {
			lastContributionIsSent = false;
		}

		if (caption_out.empty()) {
			// if no caption was built, sleep for a while
			this->lastCaption = "";
			this->lastCaptionTime = now;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		if (caption_out == lastCaption) {
			// if it has been max_time since the last caption - clear the presentation queue
			const auto duration = std::chrono::duration_cast<std::chrono::seconds>(
				now - this->lastCaptionTime);
			if (this->maxTime.count() > 0) {
				if (duration > this->maxTime) {
					this->clear();
				}
			}
		} else {
			// emit the caption
			this->captionPresentationCallback(caption_out);
			this->lastCaption = caption_out;
			this->lastCaptionTime = now;
		}

		// check the input queue size (iqs), if it's big - sleep less
		std::this_thread::sleep_for(std::chrono::milliseconds(
			inputQueue.size() > 30   ? getWaitTime(SPEED_FAST)
			: inputQueue.size() > 15 ? getWaitTime(SPEED_NORMAL)
						 : getWaitTime(SPEED_SLOW)));
	}

	obs_log(LOG_INFO, "TokenBufferThread::monitor: done");
}

int TokenBufferThread::getWaitTime(TokenBufferSpeed speed) const
{
	if (this->segmentation == SEGMENTATION_WORD) {
		switch (speed) {
		case SPEED_SLOW:
			return 200;
		case SPEED_NORMAL:
			return 150;
		case SPEED_FAST:
			return 100;
		}
	} else if (this->segmentation == SEGMENTATION_TOKEN) {
		switch (speed) {
		case SPEED_SLOW:
			return 100;
		case SPEED_NORMAL:
			return 66;
		case SPEED_FAST:
			return 33;
		}
	}
	return 1000;
}
