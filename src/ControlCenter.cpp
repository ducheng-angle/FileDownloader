

#include <errno.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <sys/sysinfo.h>

#include "Log.hpp"
#include "Error.hpp"
#include "HttpDownloader.hpp"
#include "ControlCenter.hpp"
#include "Worker.hpp"

int ControlCenter::Init(std::string url, std::string protoType, std::string fileName)
{
    int err = 0;

    do {
        mProtoType = protoType;
        mURL = url;
        size_t pos = fileName.find_last_of("/");
        if (pos != std::string::npos) {
            mFileName = fileName.substr(pos + 1);
        } else {
            mFileName = fileName;
        }

        /* curl global init */
        CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
        if (CURLE_OK != res) {
            err = E_CURL_GLOBAL_INIT;
            ERROR_LOG("curl global init failed, error: " << err);
            break;
        }

        /* get file size */
        err = GetFileSize();
        if (0 != err) {
            ERROR_LOG("Get File size failed, error: " << err);
            break;
        }

        /* open file TODO */
        mFD = open(mFileName.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (mFD < 0) {
            err = errno;
            ERROR_LOG("open file failed, error: " << err);
        }

    } while(0);

    DEBUG_LOG("init, error: " << err
            << ", file name: " << mFileName
            << ", file size: " << mFileSize);
    return err;
}

static size_t GetHeader(char *ptr, size_t blockCount, size_t memBlockSize, void *arg)
{
    std::string *str = (std::string*)arg;
    (*str) += std::string(ptr);

    return blockCount * memBlockSize;
}

int ControlCenter::GetFileSize()
{
    int err = 0;
    CURL *handle= curl_easy_init();
    double cl = 0;
    std::string header;
    CURLcode ecode;

    do {
        if (NULL == handle) {
            err = E_CURL_EASY_INIT;
            break;
        }
        curl_easy_setopt(handle, CURLOPT_URL, mURL.c_str());
        curl_easy_setopt(handle, CURLOPT_HEADER, 1);
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, GetHeader);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &header);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

        ecode = curl_easy_perform(handle);
        if (ecode == CURLE_OK) {
            ecode = curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl);
            if (CURLE_OK != ecode) {
                err = E_CURL_GET_FILE_SIZE;
            }
            mFileSize = cl;
        } else {
            err = E_CURL_GET_FILE_SIZE;
            break;
        }

        if (0 != err) {
            ERROR_LOG("GetFileSize, failed, error: " << curl_easy_strerror(ecode)); 
        }

        /* check if server support range download */
        if (header.find("Accept-Ranges: bytes") != std::string::npos) {
            DEBUG_LOG("Range Download is supported on file server!!!");
            mServerSupportRange = true;
        }

    } while(0);

    return err;
}

uint32_t ControlCenter::GetWorkerCount()
{
    /* TODO get cpu count and determine it */
    uint32_t count = 0;

    if (mServerSupportRange) {
        count = get_nprocs() * 2;
        if (mFileSize < (count * ControlCenter::ChunkSize)) {
            count = mFileSize / ControlCenter::ChunkSize + 1;
        }
    } else {
        count = 1;
        DEBUG_LOG("Server not support range download, so create 1 worker!");
    }

    DEBUG_LOG("Worker count is: " << count);
    return count;
}

Downloader *ControlCenter::NewDownloader()
{
    Downloader *dl = NULL;

    /* new specific downloader according to proto type */
    if (mProtoType == "HTTP") {
        dl = new HttpDownloader(mURL);
    } 

    return dl;
}

int ControlCenter::CreateWorkers()
{
    int err = 0;
    uint32_t workerCount = GetWorkerCount();
    Worker *wk = NULL;
    Downloader *dl = NULL;
    off_t offset = 0;
    off_t beginOffset = 0;
    size_t size = 0;

    Job *job = NULL;
    std::deque<Job*> jobQ;
    FileInfo info;
    info.fd = mFD;
    info.mErr = 0;

    uint32_t chunkCount = 0;
    for (uint32_t i = 0; i < workerCount; ++i) {
        jobQ.clear();
        wk = new Worker(this);
        mWorkers.push_back(wk);

        dl = NewDownloader();
        assert(NULL != dl);
        mDownLoaders.push_back(dl);

        err = dl->Init();
        if (0 != err) {
            break;
        }

        beginOffset = i * (mFileSize / workerCount);
        if (i < workerCount - 1) {
            size = mFileSize / workerCount;
        } else {
            size = mFileSize - (mFileSize / workerCount) * (workerCount - 1);
        }

        if (size % ControlCenter::ChunkSize == 0) {
            chunkCount = size / ControlCenter::ChunkSize;
        } else {
            chunkCount = size / ControlCenter::ChunkSize + 1;
        }

        for (uint32_t j = 0; j < chunkCount; ++j) {
            info.offset = beginOffset + j * ControlCenter::ChunkSize;

            if (j < chunkCount - 1) {
                info.size = ControlCenter::ChunkSize;
            } else {
                info.size = size - (chunkCount - 1) * ControlCenter::ChunkSize;
            }

            job = new Job(dl);
            job->SetFileInfo(info);
            jobQ.push_back(job);
        }

        DEBUG_LOG("Start worker-" << i << ", jobQ size: " << jobQ.size());

        wk->SetJobs(jobQ);
        err = wk->Start();
        if (0 != err) {
            break;
        }

    }

    return err;
}

void ControlCenter::WaitStop()
{
    for(uint32_t i = 0; i < mWorkers.size(); ++i) {
        mWorkers[i]->WaitStop();
        delete mWorkers[i];
    }
    mWorkers.clear();

    if (mFD != -1) {
        close(mFD);
    }
}

int ControlCenter::StartWork()
{
    int err = 0;

    do {
        err = CreateWorkers();
        if (0 != err) {
            break;
        }

        WaitStop();

        err = GetExitCode();
        if (0 != err) {
            break;
        }
    } while(0);

    DEBUG_LOG("StartWork, error: " << err);
    return err;
}


