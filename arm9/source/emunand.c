/*
*   This file is part of Luma3DS
*   Copyright (C) 2016-2021 Aurora Wright, TuxSH
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

/*
*   Code for locating the SDMMC struct by Normmatt
*/


#include "emunand.h"
#include "memory.h"
#include "utils.h"
#include "fatfs/sdmmc/sdmmc.h"
#include "large_patches.h"

u32 emuOffset,
    emuHeader;

void locateEmuNand(FirmwareSource *nandType, u32 *emunandIndex, bool configureCtrNandParams)
{
    static u8 __attribute__((aligned(4))) temp[0x200];
    static u32 nandSize = 0,
               fatStart;

    if(!nandSize)
    {
        nandSize = getMMCDevice(0)->total_size;
        sdmmc_sdcard_readsectors(0, 1, temp);
        fatStart = *(u32 *)(temp + 0x1C6); //First sector of the FAT partition
    }

    /*if (*nandType == FIRMWARE_SYSNAND)
        return;*/

    for(u32 i = 0; i < 3; i++)  // Test the different kinds of multi-EmuNAND there are, unless we are looking for the first one
    {
        static const u32 roundedMinsizes[] = {0x1D8000, 0x26E000};

        u32 nandOffset;
        switch(i)
        {
            case 1:
                nandOffset = ROUND_TO_4MB(nandSize + 1); //"Default" layout
                break;
            case 2:
                nandOffset = roundedMinsizes[ISN3DS ? 1 : 0]; //"Minsize" layout
                break;
            case 0:
                nandOffset = nandSize > 0x200000 ? 0x400000 : 0x200000; //"Legacy" layout
                break;
        }

        nandOffset *= *emunandIndex; // always 0 for 1st EmuNAND

        if(fatStart >= nandOffset + roundedMinsizes[ISN3DS ? 1 : 0])
        {
            //Check for RedNAND
            if(!sdmmc_sdcard_readsectors(nandOffset + 1, 1, temp) && memcmp(temp + 0x100, "NCSD", 4) == 0)
            {
                if (configureCtrNandParams)
                {
                    emuOffset = nandOffset + 1;
                    emuHeader = 0;
                }
                return;
            }

            //Check for Gateway EmuNAND
            else if(i != 2 && !sdmmc_sdcard_readsectors(nandOffset + nandSize, 1, temp) && memcmp(temp + 0x100, "NCSD", 4) == 0)
            {
                if (configureCtrNandParams)
                {
                    emuOffset = nandOffset;
                    emuHeader = nandSize;
                }
                return;
            }
        }

        if(*emunandIndex == 0) break; // See above comments
    }

    //Fallback to the first EmuNAND if there's no second/third/fourth one, or to SysNAND if there isn't any
    if(*emunandIndex != 0)
    {
        *emunandIndex = 0;
        locateEmuNand(nandType, emunandIndex, configureCtrNandParams);
    }
    else *nandType = FIRMWARE_SYSNAND;
}

static inline u32 getOldSdmmc(u32 *sdmmc, u32 firmVersion)
{
    switch(firmVersion)
    {
        case 0x18:
            *sdmmc = 0x080D91D8;
            break;
        case 0x1D:
        case 0x1F:
            *sdmmc = 0x080D8CD0;
            break;
        default:
            return 1;
    }

    return 0;
}

static inline u32 getSdmmc(u8 *pos, u32 size, u32 *sdmmc)
{
    //Look for struct code
    static const u8 pattern[] = {0x21, 0x20, 0x18, 0x20};

    const u8 *off = memsearch(pos, pattern, size, sizeof(pattern));

    if(off == NULL) return 1;

    *sdmmc = *(u32 *)(off + 9) + *(u32 *)(off + 0xD);

    return 0;
}

static inline u32 getTwlSdmmc(u8 *pos, u32 size, u32 *sdmmc)
{
    static const u8 pattern[] = {0xF2, 0xD0, 0x12, 0x48},
                    pattern2[] = {0x3D, 0x18, 0x28, 0x79};

    const u32 *off = (u32 *)memsearch(pos, pattern, size, sizeof(pattern));
    const u16 *off2 = (u16 *)memsearch(pos, pattern2, size, sizeof(pattern2));

    if(off == NULL || off2 == NULL) return 1;

    *sdmmc = *(off + 0x13) + *(u32 *)(off2 + (*(off2 - 1) & 0xFF) * 2);

    return 0;
}

static inline u32 patchNandRw(u8 *pos, u32 size, u32 hookAddr)

{
    //Look for read/write code
    static const u8 pattern[] = {0x1E, 0x00, 0xC8, 0x05};

    u16 *readOffset = (u16 *)((u32)memsearch(pos, pattern, size, sizeof(pattern)) | 2);

    if(readOffset == NULL) return 1;

    readOffset -= 3;

    u16 *writeOffset = (u16 *)((u32)memsearch((u8 *)(readOffset + 5), pattern, 0x100, sizeof(pattern)) | 2);

    if(writeOffset == NULL) return 1;

    writeOffset -= 3;
    *readOffset = *writeOffset = 0x4C00;
    readOffset[1] = writeOffset[1] = 0x47A0;
    ((u32 *)writeOffset)[1] = ((u32 *)readOffset)[1] = hookAddr;

    return 0;
}

u32 patchEmuNand(u8 *process9Offset, u32 process9Size, u32 firmVersion, bool twl)
{
    u32 ret = 0;

    //Add the data of the found EmuNAND
    emunandPatchNandOffset = emuOffset;
    emunandPatchNcsdHeaderOffset = emuHeader;

    //Find and add the SDMMC struct
    u32 sdmmc;
    ret += twl ? getTwlSdmmc(process9Offset, process9Size, &sdmmc) : !ISN3DS && firmVersion < 0x25 ? getOldSdmmc(&sdmmc, firmVersion) : getSdmmc(process9Offset, process9Size, &sdmmc);
    if(!ret) emunandPatchSdmmcStructPtr = sdmmc;

    //Add EmuNAND hooks
    u32 offsetAllign = (u32)(emunandPatch) | 1;
    ret += patchNandRw(process9Offset, process9Size, offsetAllign);

    return ret;
}
