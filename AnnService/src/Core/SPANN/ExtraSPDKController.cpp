// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inc/Core/SPANN/ExtraSPDKController.h"

namespace SPTAG::SPANN
{

thread_local struct SPDKIO::BlockController::IoContext SPDKIO::BlockController::m_currIoContext;
int SPDKIO::BlockController::m_ssdInflight = 0;
std::unique_ptr<char[]> SPDKIO::BlockController::m_memBuffer;

void SPDKIO::BlockController::SpdkBdevEventCallback(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx) {
    fprintf(stderr, "SpdkBdevEventCallback: supported bdev event type %d\n", type);
}

void SPDKIO::BlockController::SpdkBdevIoCallback(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    SubIoRequest* currSubIo = (SubIoRequest *)cb_arg;
    if (success) {
        spdk_bdev_free_io(bdev_io);
        currSubIo->completed_sub_io_requests->push(currSubIo);
        m_ssdInflight--;
        SpdkIoLoop(currSubIo->ctrl);
    } else {
        fprintf(stderr, "SpdkBdevIoCallback: I/O failed %p\n", currSubIo);
        spdk_app_stop(-1);
    }
}

void SPDKIO::BlockController::SpdkStop(void *arg) {
    SPDKIO::BlockController* ctrl = (SPDKIO::BlockController *)arg;
    // Close I/O channel and bdev
    spdk_put_io_channel(ctrl->m_ssdSpdkBdevIoChannel);
    spdk_bdev_close(ctrl->m_ssdSpdkBdevDesc);
    fprintf(stdout, "SPDKIO::BlockController::SpdkStop: finalized\n");
}

void SPDKIO::BlockController::SpdkIoLoop(void *arg) {
    SPDKIO::BlockController* ctrl = (SPDKIO::BlockController *)arg;
    int rc = 0;
    SubIoRequest* currSubIo = nullptr;
    while (!ctrl->m_ssdSpdkThreadExiting) {
        if (ctrl->m_submittedSubIoRequests.try_pop(currSubIo)) {
            if (currSubIo->is_read) {
                rc = spdk_bdev_read(
                    ctrl->m_ssdSpdkBdevDesc, ctrl->m_ssdSpdkBdevIoChannel,
                    currSubIo->dma_buff, currSubIo->offset, PageSize, SpdkBdevIoCallback, currSubIo);
            } else {
                rc = spdk_bdev_write(
                    ctrl->m_ssdSpdkBdevDesc, ctrl->m_ssdSpdkBdevIoChannel,
                    currSubIo->dma_buff, currSubIo->offset, PageSize, SpdkBdevIoCallback, currSubIo);
            }
            if (rc && rc != -ENOMEM) {
                fprintf(stderr, "SPDKIO::BlockController::SpdkStart %s failed: %d, shutting down\n",
                    currSubIo->is_read ? "spdk_bdev_read" : "spdk_bdev_write", rc);
                spdk_app_stop(-1);
                break;
            } else {
                m_ssdInflight++;
            }
        } else if (m_ssdInflight) {
            break;
        }
    }
    if (ctrl->m_ssdSpdkThreadExiting) {
        SpdkStop(ctrl);
    }
}

void SPDKIO::BlockController::SpdkStart(void *arg) {
    SPDKIO::BlockController* ctrl = (SPDKIO::BlockController *)arg;

    fprintf(stdout, "SPDKIO::BlockController::SpdkStart: using bdev %s\n", ctrl->m_ssdSpdkBdevName);

    int rc = 0;
    ctrl->m_ssdSpdkBdev = NULL;
    ctrl->m_ssdSpdkBdevDesc = NULL;

    // Open bdev
    rc = spdk_bdev_open_ext(ctrl->m_ssdSpdkBdevName, true, SpdkBdevEventCallback, NULL, &ctrl->m_ssdSpdkBdevDesc);
    if (rc) {
        fprintf(stderr, "SPDKIO::BlockController::SpdkStart: spdk_bdev_open_ext failed, %d\n", rc);
        ctrl->m_ssdSpdkThreadStartFailed = true;
        spdk_app_stop(-1);
        return;
    }
    ctrl->m_ssdSpdkBdev = spdk_bdev_desc_get_bdev(ctrl->m_ssdSpdkBdevDesc);

    // Open I/O channel
    ctrl->m_ssdSpdkBdevIoChannel = spdk_bdev_get_io_channel(ctrl->m_ssdSpdkBdevDesc);
    if (ctrl->m_ssdSpdkBdevIoChannel == NULL) {
        fprintf(stderr, "SPDKIO::BlockController::SpdkStart: spdk_bdev_get_io_channel failed\n");
        spdk_bdev_close(ctrl->m_ssdSpdkBdevDesc);
        ctrl->m_ssdSpdkThreadStartFailed = true;
        spdk_app_stop(-1);
        return;
    }

    ctrl->m_ssdSpdkThreadReady = true;
    m_ssdInflight = 0;

    SpdkIoLoop(ctrl);
}

void* SPDKIO::BlockController::InitializeSpdk(void *arg) {
    SPDKIO::BlockController* ctrl = (SPDKIO::BlockController *)arg;

    struct spdk_app_opts opts;
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "spfresh";
    const char* spdkConf = getenv(kSpdkConfEnv);
    opts.json_config_file = spdkConf ? spdkConf : "";
    const char* spdkBdevName = getenv(kSpdkBdevNameEnv);
    ctrl->m_ssdSpdkBdevName = spdkBdevName ? spdkBdevName : "";
    const char* spdkIoDepth = getenv(kSpdkIoDepth);
    if (spdkIoDepth) ctrl->m_ssdSpdkIoDepth = atoi(spdkIoDepth);

    int rc;
    rc = spdk_app_start(&opts, &SPTAG::SPANN::SPDKIO::BlockController::SpdkStart, arg);
    if (rc) {
        ctrl->m_ssdSpdkThreadStartFailed = true;
    } else {
        spdk_app_fini();
    }
    pthread_exit(NULL);
}

bool SPDKIO::BlockController::Initialize() {
    std::lock_guard<std::mutex> lock(m_initMutex);
    m_numInitCalled++;

    const char* useMemImplEnvStr = getenv(kUseMemImplEnv);
    m_useMemImpl = useMemImplEnvStr && !strcmp(useMemImplEnvStr, "1");
    const char* useSsdImplEnvStr = getenv(kUseSsdImplEnv);
    m_useSsdImpl = useSsdImplEnvStr && !strcmp(useSsdImplEnvStr, "1");
    if (m_useMemImpl) {
        if (m_numInitCalled == 1) {
            if (m_memBuffer == nullptr) {
                m_memBuffer.reset(new char[kMemImplMaxNumBlocks * PageSize]);
            }
            for (AddressType i = 0; i < kMemImplMaxNumBlocks; i++) {
                m_blockAddresses.push(i);
            }
        }
        return true;
    } else if (m_useSsdImpl) {
        if (m_numInitCalled == 1) {
            for (AddressType i = 0; i < kSsdImplMaxNumBlocks; i++) {
                m_blockAddresses.push(i);
            }
            pthread_create(&m_ssdSpdkTid, NULL, &InitializeSpdk, this);
            while (!m_ssdSpdkThreadReady && !m_ssdSpdkThreadStartFailed);
            if (m_ssdSpdkThreadStartFailed) {
                fprintf(stderr, "SPDKIO::BlockController::Initialize failed\n");
                return false;
            }
        }
        // Create sub I/O request pool
        m_currIoContext.sub_io_requests.resize(m_ssdSpdkIoDepth);
        m_currIoContext.in_flight = 0;
        uint32_t buf_align;
        buf_align = spdk_bdev_get_buf_align(m_ssdSpdkBdev);
        for (auto &sr : m_currIoContext.sub_io_requests) {
            sr.completed_sub_io_requests = &(m_currIoContext.completed_sub_io_requests);
            sr.app_buff = nullptr;
            sr.dma_buff = spdk_dma_zmalloc(PageSize, buf_align, NULL);
            sr.ctrl = this;
            m_currIoContext.free_sub_io_requests.push_back(&sr);
        }
        return true;
    } else {
        fprintf(stderr, "SPDKIO::BlockController::Initialize failed\n");
        return false;
    }
}

// get p_size blocks from front, and fill in p_data array
bool SPDKIO::BlockController::GetBlocks(AddressType* p_data, int p_size) {
    AddressType currBlockAddress = 0;
    if (m_useMemImpl || m_useSsdImpl) {
        for (int i = 0; i < p_size; i++) {
            while (!m_blockAddresses.try_pop(currBlockAddress));
            p_data[i] = currBlockAddress;
        }
        return true;
    } else {
        fprintf(stderr, "SPDKIO::BlockController::GetBlocks failed\n");
        return false;
    }
}

// release p_size blocks, put them at the end of the queue
bool SPDKIO::BlockController::ReleaseBlocks(AddressType* p_data, int p_size) {
    if (m_useMemImpl || m_useSsdImpl) {
        for (int i = 0; i < p_size; i++) {
            m_blockAddresses.push(p_data[i]);
        }
        return true;
    } else {
        fprintf(stderr, "SPDKIO::BlockController::ReleaseBlocks failed\n");
        return false;
    }
}

// read a posting list. p_data[0] is the total data size,
// p_data[1], p_data[2], ..., p_data[((p_data[0] + PageSize - 1) >> PageSizeEx)] are the addresses of the blocks
// concat all the block contents together into p_value string.
bool SPDKIO::BlockController::ReadBlocks(AddressType* p_data, std::string* p_value, const std::chrono::microseconds &timeout) {
    if (m_useMemImpl) {
        p_value->resize(p_data[0]);
        AddressType currOffset = 0;
        AddressType dataIdx = 1;
        while (currOffset < p_data[0]) {
            AddressType readSize = (p_data[0] - currOffset) < PageSize ? (p_data[0] - currOffset) : PageSize;
            memcpy(p_value->data() + currOffset, m_memBuffer.get() + p_data[dataIdx] * PageSize, readSize);
            currOffset += PageSize;
            dataIdx++;
        }
        return true;
    } else if (m_useSsdImpl) {
        p_value->resize(p_data[0]);
        AddressType currOffset = 0;
        AddressType dataIdx = 1;
        SubIoRequest* currSubIo;

        // Clear timeout I/Os
        while (m_currIoContext.in_flight) {
            if (m_currIoContext.completed_sub_io_requests.try_pop(currSubIo)) {
                currSubIo->app_buff = nullptr;
                m_currIoContext.free_sub_io_requests.push_back(currSubIo);
                m_currIoContext.in_flight--;
            }
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        // Submit all I/Os
        while (currOffset < p_data[0] || m_currIoContext.in_flight) {
            auto t2 = std::chrono::high_resolution_clock::now();
            if (std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1) > timeout) {
                break;
            }
            // Try submit
            if (currOffset < p_data[0] && m_currIoContext.free_sub_io_requests.size()) {
                currSubIo = m_currIoContext.free_sub_io_requests.back();
                m_currIoContext.free_sub_io_requests.pop_back();
                currSubIo->app_buff = p_value->data() + currOffset;
                currSubIo->real_size = (p_data[0] - currOffset) < PageSize ? (p_data[0] - currOffset) : PageSize;
                currSubIo->is_read = true;
                currSubIo->offset = p_data[dataIdx] * PageSize;
                m_submittedSubIoRequests.push(currSubIo);
                currOffset += PageSize;
                dataIdx++;
                m_currIoContext.in_flight++;
            }
            // Try complete
            if (m_currIoContext.in_flight && m_currIoContext.completed_sub_io_requests.try_pop(currSubIo)) {
                memcpy(currSubIo->app_buff, currSubIo->dma_buff, currSubIo->real_size);
                currSubIo->app_buff = nullptr;
                m_currIoContext.free_sub_io_requests.push_back(currSubIo);
                m_currIoContext.in_flight--;
            }
        }
        return true;
    } else {
        fprintf(stderr, "SPDKIO::BlockController::ReadBlocks single failed\n");
        return false;
    }
}

// parallel read a list of posting lists.
bool SPDKIO::BlockController::ReadBlocks(std::vector<AddressType*>& p_data, std::vector<std::string>* p_values, const std::chrono::microseconds &timeout) {
    if (m_useMemImpl) {
        p_values->resize(p_data.size());
        for (size_t i = 0; i < p_data.size(); i++) {
            ReadBlocks(p_data[i], &((*p_values)[i]));
        }
        return true;
    } else if (m_useSsdImpl) {
        // TODO: optimize
        auto t1 = std::chrono::high_resolution_clock::now();
        p_values->resize(p_data.size());
        for (size_t i = 0; i < p_data.size(); i++) {
            auto t2 = std::chrono::high_resolution_clock::now();
            auto interval = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
            if (interval > timeout) {
                break;
            } else {
                auto rest = timeout - interval;
                ReadBlocks(p_data[i], &((*p_values)[i]), rest);
            }
        }
        return true;
    } else {
        fprintf(stderr, "SPDKIO::BlockController::ReadBlocks batch failed\n");
        return false;
    }
}

// write p_value into p_size blocks start from p_data
bool SPDKIO::BlockController::WriteBlocks(AddressType* p_data, int p_size, const std::string& p_value) {
    if (m_useMemImpl) {
        for (int i = 0; i < p_size; i++) {
            memcpy(m_memBuffer.get() + p_data[i] * PageSize, p_value.data() + i * PageSize, PageSize);
        }
        return true;
    } else if (m_useSsdImpl) {
        AddressType currBlockIdx = 0;
        int inflight = 0;
        SubIoRequest* currSubIo;
        // Submit all I/Os
        while (currBlockIdx < p_size || inflight) {
            // Try submit
            if (currBlockIdx < p_size && m_currIoContext.free_sub_io_requests.size()) {
                currSubIo = m_currIoContext.free_sub_io_requests.back();
                m_currIoContext.free_sub_io_requests.pop_back();
                currSubIo->app_buff = const_cast<char *>(p_value.data()) + currBlockIdx * PageSize;
                currSubIo->real_size = PageSize;
                currSubIo->is_read = false;
                currSubIo->offset = p_data[currBlockIdx] * PageSize;
                memcpy(currSubIo->dma_buff, currSubIo->app_buff, currSubIo->real_size);
                m_submittedSubIoRequests.push(currSubIo);
                currBlockIdx++;
                inflight++;
            }
            // Try complete
            if (inflight && m_currIoContext.completed_sub_io_requests.try_pop(currSubIo)) {
                currSubIo->app_buff = nullptr;
                m_currIoContext.free_sub_io_requests.push_back(currSubIo);
                inflight--;
            }
        }
        return true;
    } else {
        fprintf(stderr, "SPDKIO::BlockController::ReadBlocks single failed\n");
        return false;
    }
}

bool SPDKIO::BlockController::ShutDown() {
    std::lock_guard<std::mutex> lock(m_initMutex);
    m_numInitCalled--;

    if (m_useMemImpl) {
        if (m_numInitCalled == 0) {
            while (!m_blockAddresses.empty()) {
                AddressType currBlockAddress;
                m_blockAddresses.try_pop(currBlockAddress);
            }
        }
        return true;
    } else if (m_useSsdImpl) {
        if (m_numInitCalled == 0) {
            m_ssdSpdkThreadExiting = true;
            spdk_app_start_shutdown();
            pthread_join(m_ssdSpdkTid, NULL);
            while (!m_blockAddresses.empty()) {
                AddressType currBlockAddress;
                m_blockAddresses.try_pop(currBlockAddress);
            }
        }
        // Free memory buffers
        for (auto &sr : m_currIoContext.sub_io_requests) {
            sr.completed_sub_io_requests = nullptr;
            sr.app_buff = nullptr;
            spdk_free(sr.dma_buff);
            sr.dma_buff = nullptr;
        }
        m_currIoContext.free_sub_io_requests.clear();
        return true;
    } else {
        fprintf(stderr, "SPDKIO::BlockController::ShutDown failed\n");
        return false;
    }
}

}
