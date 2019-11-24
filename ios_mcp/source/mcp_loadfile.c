/* Patch for MCP_LoadFile (ioctl 0x53).
 * Also adds a sibling ioctl, 0x64, that allows setting a custom path to a main RPX
 *
 * Reference for most of the types and whatever:
 * https://github.com/exjam/decaf-emu/tree/ios/src/libdecaf/src/ios/mcp
 *
 * This is a proof of concept, and shouldn't be used until it's cleaned up a bit.
 * Co-authored by exjam, Maschell and QuarkTheAwesome
 *
 * Flow of calls:
 * - kernel loads system libraries, app rpx or user calls OSDynLoad
 * - goes to loader.elf, which will call ioctl 0x53
 *   - with fileType = LOAD_FILE_CAFE_OS
 *   - on failure, again with LOAD_FILE_PROCESS_CODE
 *   - on failure, again with LOAD_FILE_SYS_DATA_CODE
 * - each request routes here where we can do whatever
 */

#include "logger.h"
#include "ipc_types.h"
#include "../../common/ipc_defs.h"
#include "fsa.h"
#include "svc.h"
#include <string.h>

int (*const real_MCP_LoadFile)(ipcmessage* msg) = (void*)0x0501CAA8 + 1; //+1 for thumb
int (*const MCP_DoLoadFile)(const char* path, const char* path2, void* outputBuffer, uint32_t outLength, uint32_t pos, int* bytesRead, uint32_t unk) = (void*)0x05017248 + 1;
int (*const MCP_UnknownStuff)(const char* path, uint32_t pos, void* outputBuffer, uint32_t outLength, uint32_t outLength2, uint32_t unk) = (void*)0x05014CAC + 1;

static int MCP_LoadCustomFile(int target, char* path, int filesize, int fileoffset, void * out_buffer, int buffer_len, int pos);
static bool skipPPCSetup = false;
static bool didrpxfirstchunk = false;
static bool doWantReplaceRPX = false;
static bool replace_target_device = 0;
static uint32_t rep_filesize = 0;
static uint32_t rep_fileoffset = 0;
static char rpxpath[256];

#define log(fmt, ...) log_printf("%s: " fmt, __FUNCTION__, __VA_ARGS__)
#define FAIL_ON(cond, val) \
    if (cond) { \
        log(#cond " (%08X)", val); \
        return -29; \
    }

int _MCP_LoadFile_patch(ipcmessage* msg) {

    FAIL_ON(!msg->ioctl.buffer_in, 0);
    FAIL_ON(msg->ioctl.length_in != 0x12D8, msg->ioctl.length_in);
    FAIL_ON(!msg->ioctl.buffer_io, 0);
    FAIL_ON(!msg->ioctl.length_io, 0);

    MCPLoadFileRequest* request = (MCPLoadFileRequest*)msg->ioctl.buffer_in;

    //dumpHex(request, sizeof(MCPLoadFileRequest));
    //DEBUG_FUNCTION_LINE("msg->ioctl.buffer_io = %p, msg->ioctl.length_io = 0x%X\n", msg->ioctl.buffer_io, msg->ioctl.length_io);
    //DEBUG_FUNCTION_LINE("request->type = %d, request->pos = %d, request->name = \"%s\"\n", request->type, request->pos, request->name);

    int replace_target = replace_target_device;
    int replace_filesize = rep_filesize;
    int replace_fileoffset = rep_fileoffset;
    char * replace_path = rpxpath;

    if(strncmp(request->name, "men.rpx", strlen("men.rpx")) == 0 && !skipPPCSetup) {
        // At startup we want to hook into the Wii U Menu by replacing the men.rpx with a file from the SD Card
        // The replacement may restart the application to execute a kernel exploit.
        // The men.rpx is hooked until the "IPC_CUSTOM_MEN_RPX_HOOK_COMPLETED" command is passed to IOCTL 0x100.
        // If the loading of the replacement file fails, the Wii U Menu is loaded normally.
        replace_target = LOAD_FILE_TARGET_SD_CARD;
        replace_path = "wiiu/root.rpx";

        int result = MCP_LoadCustomFile(replace_target, replace_path, 0, 0, msg->ioctl.buffer_io, msg->ioctl.length_io, request->pos);

        if (result >= 0) {
            return result;
        } else {
            // on error don't try it again.
            skipPPCSetup = true;
        }
    } else if(strncmp(request->name, "safe.rpx", strlen("safe.rpx")) == 0) {
        if (request->pos == 0) {
            didrpxfirstchunk = false;
        }

        // if we don't explicitly replace files, we do want replace the Healt and Safety app with the HBL
        if(!doWantReplaceRPX) {
            replace_path = "wiiu/apps/homebrew_launcher/homebrew_launcher.rpx";
            replace_target = LOAD_FILE_TARGET_SD_CARD;
            //doWantReplaceXML = false;
            doWantReplaceRPX = true;
            replace_filesize = 0; // unknown
            replace_fileoffset = 0;
        }
    }

    if(replace_path != NULL && strlen(replace_path) > 0) {
        if (!didrpxfirstchunk || request->pos > 0) {
            doWantReplaceRPX = false; // Only replace it once.
            int result = MCP_LoadCustomFile(replace_target, replace_path, replace_filesize, replace_fileoffset, msg->ioctl.buffer_io, msg->ioctl.length_io, request->pos);

            if (result >= 0) {
                if (request->pos == 0) {
                    didrpxfirstchunk = true;
                }
                return result;
            } else {
                // TODO, what happens if we already replaced the app/cos xml files and then the loading fails?
            }
        }
    }

    return real_MCP_LoadFile(msg);
}


// Set filesize to 0 if unknown.
static int MCP_LoadCustomFile(int target, char* path, int filesize, int fileoffset, void * buffer_out, int buffer_len, int pos) {
    if(path == NULL) {
        return 0;
    }


    char filepath[256];
    memset(filepath,0,sizeof(filepath));
    strncpy(filepath, path, sizeof(filepath) -1);

    if(target == LOAD_FILE_TARGET_SD_CARD) {
        char mountpath[] = "/vol/storage_iosu_homebrew";
        int fsa_h = svcOpen("/dev/fsa", 0);
        FSA_Mount(fsa_h, "/dev/sdcard01", mountpath, 2, NULL, 0);
        svcClose(fsa_h);
        strncpy(filepath,mountpath,sizeof(filepath) -1);
        strncat(filepath,"/",(sizeof(filepath) - 1) - strlen(filepath));
        strncat(filepath,path,(sizeof(filepath) - 1) - strlen(filepath));
    }


    DEBUG_FUNCTION_LINE("Load custom path \"%s\"\n", filepath);

    if(filesize > 0 && (pos + fileoffset >  filesize)) {
        return 0;
    }

    /*  TODO: If this fails, try last argument as 1 */
    int bytesRead = 0;
    int result = MCP_DoLoadFile(filepath, NULL, buffer_out, buffer_len, pos + fileoffset, &bytesRead, 0);
    //log("MCP_DoLoadFile returned %d, bytesRead = %d pos %d \n", result, bytesRead, pos + fileoffset);


    if (result >= 0) {
        if (!bytesRead) {
            return 0;
        }

        /*  TODO: If this fails, try last argument as 1 */
        result = MCP_UnknownStuff(filepath, pos + fileoffset, buffer_out, buffer_len, buffer_len, 0);
        //log("MCP_UnknownStuff returned %d\n", result);

        if (result >= 0) {
            if(filesize > 0 && (bytesRead + pos > filesize)) {
                return filesize - pos;
            }
            return bytesRead;
        }
    }
    return result;
}


/*  RPX replacement! Call this ioctl to replace the next loaded RPX with an arbitrary path.
    DO NOT RETURN 0, this affects the codepaths back in the IOSU code */
int _MCP_ioctl100_patch(ipcmessage* msg) {
    /*  Give some method to detect this ioctl's prescence, even if the other args are bad */
    if (msg->ioctl.buffer_io && msg->ioctl.length_io >= sizeof(u32)) {
        *(u32*)msg->ioctl.buffer_io = 1;
    }

    FAIL_ON(!msg->ioctl.buffer_in, 0);
    FAIL_ON(!msg->ioctl.length_in, 0);

    if(msg->ioctl.buffer_in && msg->ioctl.length_in >= 4) {
        int command = msg->ioctl.buffer_in[0];

        switch(command) {
        case IPC_CUSTOM_LOG_STRING: {
            //DEBUG_FUNCTION_LINE("IPC_CUSTOM_LOG_STRING\n");
            if(msg->ioctl.length_in > 4) {
                char * str_ptr = (char * ) &msg->ioctl.buffer_in[0x04 / 0x04];
                str_ptr[msg->ioctl.length_in - 0x04 - 1] = 0;
                log_printf("%s",str_ptr);
            }
            return 1;
        }
        case IPC_CUSTOM_META_XML_SWAP_REQUIRED: {
            //DEBUG_FUNCTION_LINE("IPC_CUSTOM_META_XML_SWAP_REQUIRED\n");
            /*if(doWantReplaceXML) {
                msg->ioctl.buffer_io[0] = 10;
            } else {
                msg->ioctl.buffer_io[0] = 11;
            }
            return 1;*/
        }
        case IPC_CUSTOM_MEN_RPX_HOOK_COMPLETED: {
            DEBUG_FUNCTION_LINE("IPC_CUSTOM_MEN_RPX_HOOK_COMPLETED\n");
            skipPPCSetup = true;
            return 1;
        }
        case IPC_CUSTOM_META_XML_READ: {
            if(msg->ioctl.length_io >= sizeof(ACPMetaXml)) {
                DEBUG_FUNCTION_LINE("IPC_CUSTOM_META_XML_READ\n");
                ACPMetaXml * app_ptr = (ACPMetaXml*) msg->ioctl.buffer_io;
                strncpy(app_ptr->longname_en, rpxpath, 256 - 1);
                strncpy(app_ptr->shortname_en, rpxpath, 256 - 1);
            }
            return 1;
        }
        case IPC_CUSTOM_LOAD_CUSTOM_RPX: {
            DEBUG_FUNCTION_LINE("IPC_CUSTOM_LOAD_CUSTOM_RPX\n");

            if(msg->ioctl.length_in >= 0x110) {
                int target = msg->ioctl.buffer_in[0x04/0x04];
                int filesize = msg->ioctl.buffer_in[0x08/0x04];
                int fileoffset = msg->ioctl.buffer_in[0x0C/0x04];
                char * str_ptr = (char * ) &msg->ioctl.buffer_in[0x10 / 0x04];
                memset(rpxpath,0,sizeof(rpxpath));

                strncpy(rpxpath, str_ptr, 256 - 1);

                rep_filesize = filesize;
                rep_fileoffset = fileoffset;
                didrpxfirstchunk = false;
                doWantReplaceRPX = true;
                //doWantReplaceXML = true;

                DEBUG_FUNCTION_LINE("Will load %s for next title from target: %d (offset %d, filesize %d)\n", rpxpath, target,rep_fileoffset,rep_filesize);
            }
            return 1;
        }
        default: {
        }
        }
    } else {
        return -29;
    }

    /*  Signal that all went well */
    if (msg->ioctl.buffer_io && msg->ioctl.length_io >= sizeof(u32)) {
        msg->ioctl.buffer_io[0] = 2;
    }
    return 1;
}
