#include "install/remote_nsp.hpp"

#include <threads.h>
#include "data/buffered_placeholder_writer.hpp"
#include "util/title_util.hpp"
#include "error.hpp"
#include "debug.h"

namespace tin::install::nsp
{
    RemoteNSP::RemoteNSP(std::string url) :
        m_download(url)
    {

    }

    // TODO: Do verification: PFS0 magic, sizes not zero
    void RemoteNSP::RetrieveHeader()
    {
        printf("Retrieving remote NSP header...\n");

        // Retrieve the base header
        m_headerBytes.resize(sizeof(PFS0BaseHeader), 0);
        m_download.BufferDataRange(m_headerBytes.data(), 0x0, sizeof(PFS0BaseHeader), nullptr);

        LOG_DEBUG("Base header: \n");
        printBytes(nxlinkout, m_headerBytes.data(), sizeof(PFS0BaseHeader), true);

        // Retrieve the full header
        size_t remainingHeaderSize = this->GetBaseHeader()->numFiles * sizeof(PFS0FileEntry) + this->GetBaseHeader()->stringTableSize;
        m_headerBytes.resize(sizeof(PFS0BaseHeader) + remainingHeaderSize, 0);
        m_download.BufferDataRange(m_headerBytes.data() + sizeof(PFS0BaseHeader), sizeof(PFS0BaseHeader), remainingHeaderSize, nullptr);

        LOG_DEBUG("Full header: \n");
        printBytes(nxlinkout, m_headerBytes.data(), m_headerBytes.size(), true);
    }

    struct StreamFuncArgs
    {
        tin::network::HTTPDownload* download;
        tin::data::BufferedPlaceholderWriter* bufferedPlaceholderWriter;
        u64 pfs0Offset;
        u64 ncaSize;
    };

    int CurlStreamFunc(void* in)
    {
        StreamFuncArgs* args = reinterpret_cast<StreamFuncArgs*>(in);

        auto streamFunc = [&](u8* streamBuf, size_t streamBufSize) -> size_t
        {
            while (true)
            {
                if (args->bufferedPlaceholderWriter->CanAppendData(streamBufSize))
                    break;
            }

            args->bufferedPlaceholderWriter->AppendData(streamBuf, streamBufSize);
            return streamBufSize;
        };

        args->download->StreamDataRange(args->pfs0Offset, args->ncaSize, streamFunc);
        return 0;
    }

    int PlaceholderWriteFunc(void* in)
    {
        StreamFuncArgs* args = reinterpret_cast<StreamFuncArgs*>(in);

        while (!args->bufferedPlaceholderWriter->IsPlaceholderComplete())
        {
            if (args->bufferedPlaceholderWriter->CanWriteSegmentToPlaceholder())
                args->bufferedPlaceholderWriter->WriteSegmentToPlaceholder();
        }

        return 0;
    }

    void RemoteNSP::StreamToPlaceholder(nx::ncm::ContentStorage& contentStorage, NcmNcaId placeholderId)
    {
        const PFS0FileEntry* fileEntry = this->GetFileEntryByNcaId(placeholderId);
        std::string ncaFileName = this->GetFileEntryName(fileEntry);

        LOG_DEBUG("Retrieving %s\n", ncaFileName.c_str());
        size_t ncaSize = fileEntry->fileSize;

        tin::data::BufferedPlaceholderWriter bufferedPlaceholderWriter(&contentStorage, placeholderId, ncaSize);
        StreamFuncArgs args;
        args.download = &m_download;
        args.bufferedPlaceholderWriter = &bufferedPlaceholderWriter;
        args.pfs0Offset = this->GetDataOffset() + fileEntry->dataOffset;
        args.ncaSize = ncaSize;
        thrd_t curlThread;
        thrd_t writeThread;

        thrd_create(&curlThread, CurlStreamFunc, &args);
        thrd_create(&writeThread, PlaceholderWriteFunc, &args);
        
        u64 freq = armGetSystemTickFreq();
        u64 startTime = armGetSystemTick();
        size_t startSizeBuffered = 0;
        double speed = 0.0;

        while (!bufferedPlaceholderWriter.IsBufferDataComplete())
        {
            u64 newTime = armGetSystemTick();

            if (newTime - startTime >= freq)
            {
                size_t newSizeBuffered = bufferedPlaceholderWriter.GetSizeBuffered();
                double mbBuffered = (newSizeBuffered / 1000000.0) - (startSizeBuffered / 1000000.0);
                double duration = ((double)(newTime - startTime) / (double)freq);
                speed =  mbBuffered / duration;

                startTime = newTime;
                startSizeBuffered = newSizeBuffered;
            }

            u64 totalSizeMB = bufferedPlaceholderWriter.GetTotalDataSize() / 1000000;
            u64 downloadSizeMB = bufferedPlaceholderWriter.GetSizeBuffered() / 1000000;
            int downloadProgress = (int)(((double)bufferedPlaceholderWriter.GetSizeBuffered() / (double)bufferedPlaceholderWriter.GetTotalDataSize()) * 100.0);

            printf("> Download Progress: %lu/%lu MB (%i%s) (%.2f MB/s)\r", downloadSizeMB, totalSizeMB, downloadProgress, "%", speed);
            gfxFlushBuffers();
            gfxSwapBuffers();
        }

        while (!bufferedPlaceholderWriter.IsPlaceholderComplete())
        {
            u64 totalSizeMB = bufferedPlaceholderWriter.GetTotalDataSize() / 1000000;
            u64 installSizeMB = bufferedPlaceholderWriter.GetSizeWrittenToPlaceholder() / 1000000;
            int installProgress = (int)(((double)bufferedPlaceholderWriter.GetSizeWrittenToPlaceholder() / (double)bufferedPlaceholderWriter.GetTotalDataSize()) * 100.0);

            printf("> Install Progress: %lu/%lu MB (%i%s)\r", installSizeMB, totalSizeMB, installProgress, "%");
            gfxFlushBuffers();
            gfxSwapBuffers();
        }

        thrd_join(curlThread, NULL);
        thrd_join(writeThread, NULL);
    }

    const PFS0FileEntry* RemoteNSP::GetFileEntry(unsigned int index)
    {
        if (index >= this->GetBaseHeader()->numFiles)
            THROW_FORMAT("File entry index is out of bounds\n")
    
        size_t fileEntryOffset = sizeof(PFS0BaseHeader) + index * sizeof(PFS0FileEntry);

        if (m_headerBytes.size() < fileEntryOffset + sizeof(PFS0FileEntry))
            THROW_FORMAT("Header bytes is too small to get file entry!");

        return reinterpret_cast<PFS0FileEntry*>(m_headerBytes.data() + fileEntryOffset);
    }

    const PFS0FileEntry* RemoteNSP::GetFileEntryByExtension(std::string extension)
    {
        for (unsigned int i = 0; i < this->GetBaseHeader()->numFiles; i++)
        {
            const PFS0FileEntry* fileEntry = this->GetFileEntry(i);
            std::string name(this->GetFileEntryName(fileEntry));
            auto foundExtension = name.substr(name.find(".") + 1); 

            if (foundExtension == extension)
                return fileEntry;
        }

        return nullptr;
    }

    const PFS0FileEntry* RemoteNSP::GetFileEntryByName(std::string name)
    {
        for (unsigned int i = 0; i < this->GetBaseHeader()->numFiles; i++)
        {
            const PFS0FileEntry* fileEntry = this->GetFileEntry(i);
            std::string foundName(this->GetFileEntryName(fileEntry));

            if (foundName == name)
                return fileEntry;
        }

        return nullptr;
    }

    const PFS0FileEntry* RemoteNSP::GetFileEntryByNcaId(const NcmNcaId& ncaId)
    {
        const PFS0FileEntry* fileEntry = nullptr;
        std::string ncaIdStr = tin::util::GetNcaIdString(ncaId);

        if ((fileEntry = this->GetFileEntryByName(ncaIdStr + ".nca")) == nullptr)
        {
            if ((fileEntry = this->GetFileEntryByName(ncaIdStr + ".cnmt.nca")) == nullptr)
            {
                return nullptr;
            }
        }

        return fileEntry;
    }

    const char* RemoteNSP::GetFileEntryName(const PFS0FileEntry* fileEntry)
    {
        u64 stringTableStart = sizeof(PFS0BaseHeader) + this->GetBaseHeader()->numFiles * sizeof(PFS0FileEntry);
        return reinterpret_cast<const char*>(m_headerBytes.data() + stringTableStart + fileEntry->stringTableOffset);
    }

    const PFS0BaseHeader* RemoteNSP::GetBaseHeader()
    {
        if (m_headerBytes.empty())
            THROW_FORMAT("Cannot retrieve header as header bytes are empty. Have you retrieved it yet?\n");

        return reinterpret_cast<PFS0BaseHeader*>(m_headerBytes.data());
    }

    u64 RemoteNSP::GetDataOffset()
    {
        if (m_headerBytes.empty())
            THROW_FORMAT("Cannot get data offset as header is empty. Have you retrieved it yet?\n");

        return m_headerBytes.size();
    }
}