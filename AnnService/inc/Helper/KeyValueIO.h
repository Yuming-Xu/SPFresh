#ifndef _SPTAG_HELPER_KEYVALUEIO_H_
#define _SPTAG_HELPER_KEYVALUEIO_H_

#include "inc/Core/Common.h"

namespace SPTAG
{
    namespace Helper
    {
        class KeyValueIO {
        public:
            KeyValueIO() {}

            virtual ~KeyValueIO() {}

            virtual bool Initialize(const char* filePath, bool usdDirectIO) = 0;

            virtual void ShutDown() = 0;

            virtual ErrorCode Get(const std::string& key, std::string* value) = 0;

            virtual ErrorCode Get(SizeType key, std::string* value) = 0;

            virtual ErrorCode Put(const std::string& key, const std::string& value) = 0;

            virtual ErrorCode Put(SizeType key, const std::string& value) = 0;

            virtual ErrorCode Put(SizeType key, SizeType id, const void* vector, SizeType dim) = 0;

            virtual ErrorCode Delete(SizeType key) = 0;
        };
    }
}

#endif