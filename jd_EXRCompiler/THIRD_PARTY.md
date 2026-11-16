# Third-party software in jd_exrc.exe

`jd_exrc.exe` is statically linked with the libraries below. Their full licence texts are in the
`licenses/` folder of the release zip.

| Library | Used for | Licence |
|---|---|---|
| [OpenEXR](https://github.com/AcademySoftwareFoundation/openexr) | reading and writing EXR | BSD 3-Clause |
| [Imath](https://github.com/AcademySoftwareFoundation/Imath) | half floats, boxes (OpenEXR) | BSD 3-Clause |
| [libdeflate](https://github.com/ebiggers/libdeflate) | EXR ZIP compression (OpenEXR) | MIT |
| [OpenJPH](https://github.com/aous72/OpenJPH) | EXR HTJ2K compression (OpenEXR) | BSD 2-Clause |
| [Zstandard](https://github.com/facebook/zstd) | EXR compression (OpenEXR) | BSD 3-Clause (dual BSD/GPLv2; used under BSD) |
| [LibTIFF](https://gitlab.com/libtiff/libtiff) | reading TIFF | LibTIFF licence (BSD-style) |
| [zlib](https://zlib.net) | TIFF ZIP compression | zlib licence |
| [nlohmann/json](https://github.com/nlohmann/json) | job files, manifests | MIT |
| [stb](https://github.com/nothings/stb) (`stb_image`, `stb_image_write`) | reading PNG; test PNGs | MIT or public domain |

The Cryptomatte format follows the [Cryptomatte specification](https://github.com/Psyop/Cryptomatte)
(BSD 3-Clause); no code from it is included. MurmurHash3 (Austin Appleby) is public domain.
