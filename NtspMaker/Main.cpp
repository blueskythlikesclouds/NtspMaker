struct Entry
{
    uint32_t nameHash;
    uint32_t blobIndex;
    uint32_t blobCount;
    uint16_t width;
    uint16_t height;
    uint64_t nameOffset;
};

struct Blob
{
    uint64_t dataOffset;
    uint64_t dataSize;
};

struct Package
{
    uint32_t signature;
    uint32_t version;
    uint32_t entryCount;
    uint32_t blobCount;
    uint64_t headerSize;
};

struct Info
{
    uint32_t signature;
    uint32_t version;
    uint32_t reserved;
    uint32_t packageNameSize;
    uint32_t mip4x4Size;
    uint32_t mip4x4Index;
};

static uint32_t computeNameHash(const char* str) noexcept
{
    uint32_t hash = 0;

    char c;
    while ((c = *str++) != 0)
        hash = hash * 31 + c;

    return hash & 0x7FFFFFFF;
}

int main(int argc, char* argv[])
{
    std::vector<std::filesystem::path> textureFilePaths;
    std::filesystem::path ntspFilePath;

    for (int i = 1; i < argc; i++)
    {
        const std::filesystem::path path = argv[i];

        if (std::filesystem::is_directory(path))
        {
            for (const auto& entry : std::filesystem::directory_iterator(path))
            {
                if (entry.is_regular_file())
                {
                    std::filesystem::path extension = entry.path().extension();

                    if (_wcsicmp(extension.c_str(), L".dds") == 0)
                        textureFilePaths.push_back(entry.path());
                }
            }
        }
        else
        {
            std::filesystem::path extension = path.extension();

            if (_wcsicmp(extension.c_str(), L".dds") == 0)
                textureFilePaths.push_back(std::move(path));

            else if (_wcsicmp(extension.c_str(), L".ntsp") == 0)
                ntspFilePath = std::move(std::move(path));
        }
    }

    if (textureFilePaths.empty() || ntspFilePath.empty())
        return -1;

    struct TextureHolder
    {
        std::filesystem::path filePath;
        std::string name;
        uint32_t hash;
        DirectX::ScratchImage image;
    };

    std::vector<TextureHolder> textures;

    for (const auto& path : textureFilePaths)
    {
        DirectX::ScratchImage image;

        if (FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image)))
        {
            _wprintf_p(L"Failed to load %s\n", path.c_str());
            continue;
        }

        auto& texture = textures.emplace_back();

        texture.filePath = path;
        texture.name = path.stem().string();
        texture.hash = computeNameHash(texture.name.c_str());
        texture.image = std::move(image);
    }

    if (textures.empty())
        return -1;

    std::ranges::sort(textures, [](const TextureHolder& a, const TextureHolder& b) { return a.hash < b.hash; });

    size_t blobCount = 0;
    size_t namesSize = 0;

    for (const auto& texture : textures)
    {
        blobCount += texture.image.GetImageCount();
        namesSize += texture.name.size() + 1;
    }

    const size_t entriesOffset = sizeof(Package);
    const size_t blobsOffset = entriesOffset + sizeof(Entry) * textures.size();
    size_t namesOffset = blobsOffset + sizeof(Blob) * blobCount;
    const size_t headerSize = namesOffset + namesSize;

    std::vector<uint8_t> ntsp(headerSize);

    Package* package = reinterpret_cast<Package*>(ntsp.data());

    package->signature = 'NTSP';
    package->version = 1;
    package->entryCount = static_cast<uint32_t>(textures.size());
    package->blobCount = static_cast<uint32_t>(blobCount);
    package->headerSize = static_cast<uint32_t>(headerSize);

    auto curEntry = reinterpret_cast<Entry*>(ntsp.data() + entriesOffset);
    auto curBlob = reinterpret_cast<Blob*>(ntsp.data() + blobsOffset);
    auto curName = reinterpret_cast<char*>(ntsp.data() + namesOffset);

    uint32_t blobIndex = 0;
    size_t dataOffset = headerSize;

    for (const auto& texture : textures)
    {
        curEntry->nameHash = texture.hash;
        curEntry->blobIndex = blobIndex;
        curEntry->blobCount = static_cast<uint32_t>(texture.image.GetImageCount());
        curEntry->width = static_cast<uint16_t>(texture.image.GetMetadata().width);
        curEntry->height = static_cast<uint16_t>(texture.image.GetMetadata().height);
        curEntry->nameOffset = namesOffset;

        for (size_t i = 0; i < texture.image.GetImageCount(); i++)
        {
            const DirectX::Image& image = texture.image.GetImages()[i];
            curBlob->dataOffset = static_cast<uint64_t>(dataOffset);
            curBlob->dataSize = static_cast<uint64_t>(image.slicePitch);

            ++curBlob;
            ++blobIndex;
            dataOffset += image.slicePitch;
        }

        memcpy(curName, texture.name.c_str(), texture.name.size() + 1);
        namesOffset += texture.name.size() + 1;
        curName += texture.name.size() + 1;

        ++curEntry;
    }

    FILE* file = _wfopen(ntspFilePath.c_str(), L"wb");
    if (!file)
    {
        _wprintf_p(L"Failed to open %s for saving\n", ntspFilePath.c_str());
        return -1;
    }

    fwrite(ntsp.data(), 1, ntsp.size(), file);

    for (const auto& texture : textures)
    {
        for (size_t i = 0; i < texture.image.GetImageCount(); i++)
        {
            const DirectX::Image& image = texture.image.GetImages()[i];
            fwrite(image.pixels, 1, image.slicePitch, file);
        }
    }

    fclose(file);

    const std::string stem = ntspFilePath.stem().string();

    for (const auto& texture : textures)
    {
        const DirectX::Image* mip4x4Image = nullptr;
        size_t mip4x4Size = 0;
        size_t mip4x4Index = 0;

        for (size_t j = 0; j < texture.image.GetImageCount(); j++)
        {
            const DirectX::Image& image = texture.image.GetImages()[j];

            if (image.width <= 4 || image.height <= 4)
            {
                mip4x4Image = &image;
                mip4x4Size = image.slicePitch;
                mip4x4Index = j;
                break;
            }
        }

        if (!mip4x4Image)
        {
            size_t width = texture.image.GetMetadata().width;
            size_t height = texture.image.GetMetadata().height;

            while (width > 4 && height > 4)
            {
                width >>= 1;
                height >>= 1;
                ++mip4x4Index;
            }

            size_t rowPitch, slicePitch;
            DirectX::ComputePitch(texture.image.GetMetadata().format, width, height, rowPitch, slicePitch);

            mip4x4Size = slicePitch;
        }

        const size_t nameOffset = sizeof(Info);
        const size_t mip4x4Offset = nameOffset + stem.size() + 1;
        const size_t ddsHeaderOffset = mip4x4Offset + mip4x4Size;
        size_t totalSize = ddsHeaderOffset + 0x94;

        std::vector<uint8_t> ntsi(totalSize);

        Info* info = reinterpret_cast<Info*>(ntsi.data());

        info->signature = 'ISTN';
        info->version = 1;
        info->reserved = 0;
        info->packageNameSize = static_cast<uint32_t>(stem.size() + 1);
        info->mip4x4Size = static_cast<uint32_t>(mip4x4Size);
        info->mip4x4Index = static_cast<uint32_t>(mip4x4Index);

        char* packageName = reinterpret_cast<char*>(ntsi.data() + nameOffset);
        memcpy(packageName, stem.c_str(), stem.size() + 1);

        uint8_t* mip4x4Data = ntsi.data() + mip4x4Offset;

        if (mip4x4Image)
            memcpy(mip4x4Data, mip4x4Image->pixels, mip4x4Size);
        else
            memset(mip4x4Data, 0, mip4x4Size);

        DirectX::EncodeDDSHeader(texture.image.GetMetadata(), DirectX::DDS_FLAGS_NONE, ntsi.data() + ddsHeaderOffset, totalSize, totalSize);
        totalSize += ddsHeaderOffset;

        file = _wfopen(texture.filePath.c_str(), L"wb");
        if (file)
        {
            fwrite(ntsi.data(), 1, totalSize, file);
            fclose(file);
        }
        else
        {
            _wprintf_p(L"Failed to open %s for saving\n", texture.filePath.c_str());
        }
    }

    return 0;
}