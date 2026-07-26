#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shellapi.h>
#include "dramtile_store.h"

static const char *DRIVES = "GJKMNOQRTUVWXY";
static char mount_drives[26];
static int mount_count = 0;

static char find_free_drive(void) {
    for (const char *d = DRIVES; *d; d++) {
        DWORD mask = GetLogicalDrives();
        if (!(mask & (1 << (*d - 'A')))) {
            int used = 0;
            for (int i = 0; i < mount_count; i++)
                if (mount_drives[i] == *d) { used = 1; break; }
            if (!used) return *d;
        }
    }
    return 0;
}

static void list_dramtile_files(const char *dir) {
    WIN32_FIND_DATA fd;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.dramtile", dir);
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("  (no .dramtile files found)\n"); return; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        DWORD hi = fd.nFileSizeHigh;
        DWORD lo = fd.nFileSizeLow;
        double mb = ((double)hi * 4294967296.0 + lo) / 1048576.0;
        printf("  %-30s %8.1f MB\n", fd.cFileName, mb);
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

static void mount_file(const char *filepath, char drive) {
    char mp[4] = { drive, ':', '\\', 0 };
    CreateDirectoryA(mp, NULL);

    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = 0;
    else snprintf(exe, sizeof(exe), ".");

    char mounter[MAX_PATH];
    snprintf(mounter, sizeof(mounter), "%sdramtile_mount.exe", exe);

    char params[MAX_PATH * 3];
    snprintf(params, sizeof(params), "\"%s\" %c:", filepath, drive);

    printf("Mounting %s -> %c: ...\n", filepath, drive);

    /* Launch in new window so FUSE daemon doesn't block this terminal */
    SHELLEXECUTEINFOA sei = {0};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = "open";
    sei.lpFile = mounter;
    sei.lpParameters = params;
    sei.lpDirectory = exe;
    sei.nShow = SW_SHOW;

    if (ShellExecuteExA(&sei)) {
        Sleep(3000);
        DWORD mask = GetLogicalDrives();
        if (mask & (1 << (drive - 'A'))) {
            mount_drives[mount_count++] = drive;
            printf("Mounted on %c:\n", drive);
            char openpath[4] = { drive, ':', '\\', 0 };
            ShellExecuteA(NULL, "open", openpath, NULL, NULL, SW_SHOW);
        } else {
            printf("Mount started — checking again...\n");
            Sleep(3000);
            mask = GetLogicalDrives();
            if (mask & (1 << (drive - 'A'))) {
                mount_drives[mount_count++] = drive;
                printf("Mounted on %c:\n", drive);
                char openpath[4] = { drive, ':', '\\', 0 };
                ShellExecuteA(NULL, "open", openpath, NULL, NULL, SW_SHOW);
            } else {
                printf("Drive %c: not visible yet (FUSE service may need time)\n", drive);
                mount_drives[mount_count++] = drive;
            }
        }
    } else {
        printf("Mount failed! Error %lu\n", GetLastError());
    }
}

static void unmount_drive(char drive) {
    char mp[4] = { drive, ':', '\\', 0 };
    printf("Unmounting %c: ...\n", drive);
    char fuser[MAX_PATH];
    snprintf(fuser, sizeof(fuser), "C:\\Program Files (x86)\\WinFsp\\SxS\\sxs.20260705T065517Z\\bin\\launcher-x64.exe");
    
    char cmd[MAX_PATH * 2];
    snprintf(cmd, sizeof(cmd), "taskkill /F /IM dramtile_mount.exe >nul 2>&1");
    system(cmd);
    
    for (int i = 0; i < mount_count; i++) {
        if (mount_drives[i] == drive) {
            memmove(&mount_drives[i], &mount_drives[i+1], (mount_count - i - 1));
            mount_count--;
            break;
        }
    }
    printf("%c: unmounted\n", drive);
}

static void show_tensors(const char *filepath) {
    DRamTileStore store;
    memset(&store, 0, sizeof(store));
    if (dt_store_init_twin(&store, filepath, 1) != 0) {
        printf("Cannot open %s\n", filepath);
        return;
    }
    printf("\nTensors in %s:\n", filepath);
    printf("%-4s %-36s %10s %6s\n", "#", "Name", "Size", "Offset");
    printf("%-4s %-36s %10s %6s\n", "---", "----", "----", "------");
    int idx = 0;
    for (int i = 0; i < DT_HASH_SLOTS; i++) {
        if (store.hash[i].dram_addr == 0) continue;
        if (store.hash[i].dram_addr & DT_KV_FLAG) continue;
        printf("[%2d] %-36s %10zu %6zu\n",
               idx++, store.hash[i].name,
               store.hash[i].size, store.hash[i].offset);
    }
    printf("Total: %d tensors, %zu bytes\n", idx, store.used);
    /* Don't destroy twin — just unmap */
#ifdef _WIN32
    if (store.base) UnmapViewOfFile(store.base);
    if (store.hMapping) CloseHandle(store.hMapping);
    if (store.hFile) CloseHandle(store.hFile);
#endif
}

int main(int argc, char *argv[]) {
    printf("=== DRamTile Manager ===\n\n");
    
    if (argc >= 2) {
        /* dramtile_mgr <file.dramtile> [mount] */
        show_tensors(argv[1]);
        if (argc >= 3 && strcmp(argv[2], "mount") == 0) {
            char drive = find_free_drive();
            if (drive) {
                mount_file(argv[1], drive);
            } else {
                printf("No free drive letters!\n");
            }
        }
        return 0;
    }

    /* Interactive mode */
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    
    while (1) {
        printf("\n--- DRamTile Manager ---\n");
        printf("CWD: %s\n\n", cwd);
        printf("1) Browse .dramtile files\n");
        printf("2) Mount file\n");
        printf("3) Unmount drive\n");
        printf("4) List tensors in file\n");
        printf("5) Change directory\n");
        printf("0) Exit\n\n");
        printf("Choice: ");
        
        char choice[16] = "";
        fgets(choice, sizeof(choice), stdin);
        int ch = atoi(choice);
        
        if (ch == 0 && choice[0] != '0') break;
        
        switch (ch) {
        case 1:
            printf("\n.dramtile files in %s:\n", cwd);
            list_dramtile_files(cwd);
            break;
        case 2: {
            printf("File: ");
            char fp[MAX_PATH];
            fgets(fp, sizeof(fp), stdin);
            fp[strcspn(fp, "\r\n")] = 0;
            if (fp[0] == 0) break;
            char drive = find_free_drive();
            if (!drive) { printf("No free drives!\n"); break; }
            printf("Available drive: %c:\n", drive);
            mount_file(fp, drive);
            break;
        }
        case 3: {
            printf("Drive letter to unmount: ");
            char dl[16];
            fgets(dl, sizeof(dl), stdin);
            if (dl[0] >= 'a' && dl[0] <= 'z') dl[0] -= 32;
            unmount_drive(dl[0]);
            break;
        }
        case 4: {
            printf("File: ");
            char fp[MAX_PATH];
            fgets(fp, sizeof(fp), stdin);
            fp[strcspn(fp, "\r\n")] = 0;
            if (fp[0] == 0) break;
            show_tensors(fp);
            break;
        }
        case 5: {
            printf("New directory: ");
            char nd[MAX_PATH];
            fgets(nd, sizeof(nd), stdin);
            nd[strcspn(nd, "\r\n")] = 0;
            if (SetCurrentDirectoryA(nd))
                GetCurrentDirectoryA(sizeof(cwd), cwd);
            else
                printf("Invalid directory\n");
            break;
        }
        default:
            return 0;
        }
    }
    return 0;
}
