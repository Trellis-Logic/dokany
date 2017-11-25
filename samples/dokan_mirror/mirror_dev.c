
#include "../../dokan/dokan.h"
#include "../../dokan/fileinfo.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <winbase.h>
#include "mirror_dev.h"
#include <stdbool.h>

static wchar_t *g_MirrorDevDokanPath = L"\\MIRROR1";
static size_t g_MirrorDevDokanPathLength = 8;
HANDLE g_MirrorDevHandle = INVALID_HANDLE_VALUE;

/**
* Use a fixed offset with the disk - to experiment with exporting a partition only.
* I've found Windows partitions typically start at offset 0x100000
*/
#define FIXED_OFFSET 0

bool MirrorDevFileNameMatchesExpected(LPCWSTR FileName)
{
  size_t fileNameLength = wcslen(FileName);
  if (fileNameLength != g_MirrorDevDokanPathLength ||
    (wcsncmp(FileName,
      g_MirrorDevDokanPath,
      g_MirrorDevDokanPathLength) != 0))
  {
    return false;
  }
  return true;
}


static NTSTATUS DOKAN_CALLBACK
MirrorDevCreateFile(LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT SecurityContext,
	ACCESS_MASK DesiredAccess, ULONG FileAttributes,
	ULONG ShareAccess, ULONG CreateDisposition,
	ULONG CreateOptions, PDOKAN_FILE_INFO DokanFileInfo) {
  DbgPrint(L"DevCreateFile : %s\n", FileName);
  DWORD creationDisposition;
  DWORD fileAttributesAndFlags;
  ACCESS_MASK genericDesiredAccess;

  DbgPrint(L"MirrorDevCreateFile %s Share Mode 0x%x\n", FileName, ShareAccess);
  /**
  * TODO: figure out how to use this
  */
  SecurityContext = SecurityContext;

  DokanMapKernelToUserCreateFileFlags(
    FileAttributes, CreateOptions, CreateDisposition, &fileAttributesAndFlags,
    &creationDisposition);
  genericDesiredAccess = DokanMapStandardToGenericAccess(DesiredAccess);

  if (FileName == NULL)
  {
    DbgPrint(
      L"%s: invalid path.\n",
      FileName);
    return -ERROR_BAD_ARGUMENTS;
  }
  if ((genericDesiredAccess & GENERIC_WRITE) != 0)
  {
    return(-ERROR_WRITE_PROTECT);
  }
  /* Ignore the share_mode
  */
  if (creationDisposition == CREATE_NEW)
  {
    return(-ERROR_FILE_EXISTS);
  }
  else if (creationDisposition == CREATE_ALWAYS)
  {
    return(-ERROR_ALREADY_EXISTS);
  }
  else if (creationDisposition == OPEN_ALWAYS)
  {
    return(-ERROR_FILE_NOT_FOUND);
  }
  else if (creationDisposition == TRUNCATE_EXISTING)
  {
    return(-ERROR_FILE_NOT_FOUND);
  }
  else if (creationDisposition != OPEN_EXISTING)
  {
    DbgPrint(
      L"invalid creation disposition 0x%x.\n",
      creationDisposition);
    return(-ERROR_BAD_ARGUMENTS);
  }
  if (DokanFileInfo == NULL)
  {
    DbgPrint(L"Null file info\n");
    return(-ERROR_BAD_ARGUMENTS);
  }
  size_t fileNameLength = wcslen(FileName);
  if (fileNameLength == 1)
  {
    if (FileName[0] != (wchar_t) '\\')
    {
      return -ERROR_FILE_NOT_FOUND;
    }
  }
  else
  {
    if( !MirrorDevFileNameMatchesExpected(FileName) )
    {
      DbgPrint(L"Invalid file name %s\n", FileName);
      return -ERROR_FILE_NOT_FOUND;
    }
  }
  return NO_ERROR;

}

static void DOKAN_CALLBACK MirrorDevCloseFile(LPCWSTR FileName,
  PDOKAN_FILE_INFO DokanFileInfo) {
  if (DokanFileInfo->Context) {
    DbgPrint(L"CloseFile: %s\n", FileName);
    DbgPrint(L"\terror : not cleanuped file\n\n");
    CloseHandle((HANDLE)DokanFileInfo->Context);
    DokanFileInfo->Context = 0;
  }
  else {
    DbgPrint(L"Close: %s\n\n", FileName);
  }
}

#define MAX_BLOCK_SIZE 512
DWORD MirrorDevBlockSize()
{
  /**
  * TODO: actually look this up from device geometry
  */
  return MAX_BLOCK_SIZE;
}

bool MirrorDevSeekAligned(LONGLONG byteOffset, DWORD *Remainder)
{
  LARGE_INTEGER liOffset;
  DWORD blockSize = MirrorDevBlockSize();
  *Remainder= byteOffset % blockSize;
  byteOffset = byteOffset - *Remainder;
  liOffset.QuadPart = byteOffset;
  bool success = false;
  success = SetFilePointerEx(g_MirrorDevHandle, liOffset, NULL, FILE_BEGIN);
  return success;
}

/**
* Read @param LengthInBlocks blocks at @param Buffer offset into @param Buffer, at the current
* sector offset, returning true on success, false otherwise
*/
bool MirrorDevReadAlignedBlocks(LPVOID Buffer, DWORD LengthInBlocks)
{
  DWORD bytesReadThisCall;
  return ReadFile(g_MirrorDevHandle, Buffer, LengthInBlocks* MirrorDevBlockSize(), &bytesReadThisCall, NULL);
}
bool MirrorDevReadAligned(DWORD FirstBlockOffset, LPVOID Buffer, DWORD BufferLength, LPDWORD BytesReadReturn )
{
  DWORD bytesToRead = FirstBlockOffset + BufferLength;
  const DWORD blockSize = MirrorDevBlockSize();
  DWORD remainderBytes = bytesToRead % blockSize;
  UCHAR tempBuffer[MAX_BLOCK_SIZE];
  PUCHAR currentBufferOffset = Buffer;
  DWORD remainingBytes = BufferLength;
  bool success = true;
  if (FirstBlockOffset != 0)
  {
    DWORD firstBlockUsedBytes = min(BufferLength,blockSize- FirstBlockOffset);
    success = MirrorDevReadAlignedBlocks(tempBuffer, 1);
    if( !success )
    {
      DbgPrint(L"ReadFile failed with error %d reading %d bytes for first block offset %d\n", GetLastError(),blockSize,FirstBlockOffset);
    }
    else
    {
      DbgPrint(L"Read %lu aligned bytes, copied %d into buffer starting at offset %d\n", blockSize, firstBlockUsedBytes, FirstBlockOffset);
      memcpy(currentBufferOffset, &tempBuffer[FirstBlockOffset], firstBlockUsedBytes);
      currentBufferOffset += firstBlockUsedBytes;
      remainingBytes -= firstBlockUsedBytes;
    }
  }
  if (success)
  {
    if (remainingBytes >= blockSize)
    {
      DWORD alignedBlocksRead = remainingBytes/blockSize;
      DWORD alignedBytesRead = blockSize*alignedBlocksRead;
      success = MirrorDevReadAlignedBlocks(currentBufferOffset, alignedBlocksRead);
      if (!success)
      {
        DbgPrint(L"ReadFile failed with error %d reading aligned payload %lu bytes\n", GetLastError(), alignedBytesRead);
      }
      else
      {
        DbgPrint(L"Read %lu aligned blocks into buffer\n", alignedBlocksRead);
      }
      currentBufferOffset += alignedBytesRead;
      remainingBytes -= alignedBytesRead;
      if (success && remainingBytes != 0)
      {
        success = MirrorDevReadAlignedBlocks(tempBuffer, 1);
        if (!success)
        {
          DbgPrint(L"ReadFile failed with error %d reading remainder payload %d bytes\n", GetLastError(), blockSize);
        }
        else
        {
          memcpy(currentBufferOffset, &tempBuffer[0], remainingBytes);
          DbgPrint(L"Read %lu bytes,  copied %u into buffer \n", blockSize, remainingBytes);
        }
      }
    }
  }
  if (success)
  {
    *BytesReadReturn = BufferLength;
  }
  return success;
}
static NTSTATUS DOKAN_CALLBACK MirrorDevReadFile(LPCWSTR FileName, LPVOID Buffer,
  DWORD BufferLength,
  LPDWORD ReadLength,
  LONGLONG Offset,
  PDOKAN_FILE_INFO DokanFileInfo) {
  NTSTATUS rc =-ERROR_GENERIC_COMMAND_FAILED;
  Offset += FIXED_OFFSET;
  DbgPrint(L"MirrorDevReadFile %s length %lu offset %llu\n", FileName, BufferLength, Offset);
  DokanFileInfo = DokanFileInfo;
  if (!MirrorDevFileNameMatchesExpected(FileName))
  {
    DbgPrint(L"Invalid file name %s\n", FileName);
    rc=-ERROR_FILE_NOT_FOUND;
  }
  else
  {
    DWORD offsetRemainder;
    if (MirrorDevSeekAligned(Offset,&offsetRemainder))
    {
      DbgPrint(L"Seek completed successfully to offset %u with remainder %u bytes\n", Offset, offsetRemainder);
      if (MirrorDevReadAligned(offsetRemainder,Buffer, BufferLength, ReadLength))
      {
        DbgPrint(L"Read %lu bytes from device, first first 4 bytes 0x%02x%02x%02x%02x\n\n",
          *ReadLength, ((UCHAR *)Buffer)[0], ((UCHAR *)Buffer)[1], ((UCHAR *)Buffer)[2], ((UCHAR *)Buffer)[3]);
        rc = NO_ERROR;
      }
      else
      {
        DbgPrint(L"ReadFile failed with error %d\n", GetLastError());
        rc = -ERROR_READ_FAULT;
      }
    }
    else
    {
      DbgPrint(L"Seek failed with error %d\n", GetLastError());
      rc = -ERROR_SEEK_ON_DEVICE;
    }
  }
  return rc;
}



int GetMirrorDevSize(LARGE_INTEGER *size)
{
  DISK_GEOMETRY_EX geo;
  DWORD returned;
  if ( !DeviceIoControl(
    g_MirrorDevHandle,
    IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
    NULL,
    0,
    &geo,
    sizeof(DISK_GEOMETRY_EX),
    &returned,
    NULL) )
  {
    DbgPrint(L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX returned %d\n", GetLastError());
    return -ERROR_SEEK_ON_DEVICE;
  }
  else
  {
    geo.DiskSize.QuadPart -= FIXED_OFFSET;
    *size = geo.DiskSize;
    DbgPrint(L"Mirror dev size %llu bytes\n", size->QuadPart);
  }
  return NO_ERROR;
}

static NTSTATUS DOKAN_CALLBACK MirrorDevGetFileInformation(
  LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation,
  PDOKAN_FILE_INFO DokanFileInfo) {
  DbgPrint(L"MirrorDevGetFileInformation %s\n", FileName);
  size_t fileNameLength = wcslen(FileName);
  DokanFileInfo = DokanFileInfo;
  if (fileNameLength == 1)
  {
    if (FileName[0] != (wchar_t) '\\')
    {
      return -ERROR_FILE_NOT_FOUND;
    }
    HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    /* TODO set timestamps
    HandleFileInformation->ftCreationTime   = { 0, 0 };
    HandleFileInformation->ftLastAccessTime = { 0, 0 };
    HandleFileInformation->ftLastWriteTime  = { 0, 0 };
    */
  }
  else
  {
    if (!MirrorDevFileNameMatchesExpected(FileName))
    {
      DbgPrint(L"Invalid file name %s\n", FileName);
      return -ERROR_FILE_NOT_FOUND;
    }
    LARGE_INTEGER size;
    int rc = GetMirrorDevSize(&size);
    if( rc != 0 )
    {
      return rc;
    }
    HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    /* TODO set timestamps
    HandleFileInformation->ftCreationTime   = { 0, 0 };
    HandleFileInformation->ftLastAccessTime = { 0, 0 };
    HandleFileInformation->ftLastWriteTime  = { 0, 0 };
    */
    HandleFileInformation->nFileSizeHigh = size.LowPart;
    HandleFileInformation->nFileSizeLow = size.HighPart;
  }
  return NO_ERROR;
}

static NTSTATUS DOKAN_CALLBACK
MirrorDevFindFiles(LPCWSTR FileName,
  PFillFindData FillFindData, // function pointer
  PDOKAN_FILE_INFO DokanFileInfo) {
  WIN32_FIND_DATAW findData;
  DbgPrint(L"MirrorDevFindFiles %s\n",FileName);
  memset(&findData, 0, sizeof(WIN32_FIND_DATAW));
  size_t fileNameLength = wcslen(FileName);
  if ( (fileNameLength != 1) || 
     (FileName[0] != (wchar_t) '\\') )
  {
    DbgPrint(L"Invalid Filename for FindFiles %s\n", FileName);
    return -ERROR_FILE_NOT_FOUND;
  }
  wcsncpy(findData.cFileName, L".", 1);
  wcsncpy(findData.cAlternateFileName, L".", 1);
  findData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
  /* TODO set timestamps
  findData.ftCreationTime   = { 0, 0 };
  findData.ftLastAccessTime = { 0, 0 };
  findData.ftLastWriteTime  = { 0, 0 };
  */
  int rc= FillFindData(
    &findData,
    DokanFileInfo);
  if( rc != 0)
  {
    DbgPrint(L"FillFindData returned %d on . entry\n", rc);
    return -ERROR_GEN_FAILURE;
  }
  memset(&findData, 0, sizeof(WIN32_FIND_DATAW));
  wcsncpy(findData.cFileName, L"..", 1);
  wcsncpy(findData.cAlternateFileName, L"..", 1);
  findData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
  /* TODO set timestamps
  findData.ftCreationTime   = { 0, 0 };
  findData.ftLastAccessTime = { 0, 0 };
  findData.ftLastWriteTime  = { 0, 0 };
  */
  rc = FillFindData(
    &findData,
    DokanFileInfo);
  if (rc != 0)
  {
    DbgPrint(L"FillFindData returned %d on .. entry\n", rc);
    return -ERROR_GEN_FAILURE;
  }
  memset(&findData, 0, sizeof(WIN32_FIND_DATAW));
  wcsncpy(findData.cFileName, &g_MirrorDevDokanPath[1], g_MirrorDevDokanPathLength-1);
  wcsncpy(findData.cAlternateFileName, &g_MirrorDevDokanPath[1], g_MirrorDevDokanPathLength - 1);
  findData.dwFileAttributes = FILE_ATTRIBUTE_READONLY;

  LARGE_INTEGER size;
  rc = GetMirrorDevSize(&size);
  if (rc != 0)
  {
    return rc;
  }
  findData.nFileSizeLow = size.LowPart;
  findData.nFileSizeHigh = size.HighPart;
  rc = FillFindData(
    &findData,
    DokanFileInfo);
  if (rc != 0)
  {
    DbgPrint(L"FillFindData returned %d on mirror dev entry\n", rc);
    return -ERROR_GEN_FAILURE;
  }
  return NO_ERROR;

}

static NTSTATUS DOKAN_CALLBACK MirrorDevGetVolumeInformation(
  LPWSTR VolumeNameBuffer, DWORD VolumeNameSize, LPDWORD VolumeSerialNumber,
  LPDWORD MaximumComponentLength, LPDWORD FileSystemFlags,
  LPWSTR FileSystemNameBuffer, DWORD FileSystemNameSize,
  PDOKAN_FILE_INFO DokanFileInfo) {
  DokanFileInfo = DokanFileInfo;
  DbgPrint(L"MirrorDevGetVolumeInformation\n");
  wcscpy_s(VolumeNameBuffer, VolumeNameSize, &g_MirrorDevDokanPath[1]);
  if (VolumeSerialNumber)
    *VolumeSerialNumber = 0x19831116;
  if (MaximumComponentLength)
    *MaximumComponentLength = 255;
  if (FileSystemFlags)
    *FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
    FILE_UNICODE_ON_DISK |
    FILE_READ_ONLY_VOLUME;
  wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"Dokan");
  return NO_ERROR;
}
void MirrorDevFillOptions(PDOKAN_OPTIONS dokanOptions)
{
  DbgPrint(L"Forcing write protect since physical device implementation does not currently support reads\n");
  dokanOptions->Options |= DOKAN_OPTION_WRITE_PROTECT;
}
void MirrorDevFillOperations(PDOKAN_OPERATIONS dokanOperations)
{
  dokanOperations->ZwCreateFile = MirrorDevCreateFile;
  dokanOperations->CloseFile = MirrorDevCloseFile;
  dokanOperations->ReadFile = MirrorDevReadFile;
  dokanOperations->GetFileInformation = MirrorDevGetFileInformation;
  dokanOperations->FindFiles = MirrorDevFindFiles;
  dokanOperations->GetVolumeInformationW = MirrorDevGetVolumeInformation;
}

int MirrorDevInit(LPWSTR PhysicalDrive, PDOKAN_OPTIONS Options, PDOKAN_OPERATIONS Operations)
{
  MirrorDevFillOptions(Options);
  MirrorDevFillOperations(Operations);
  g_MirrorDevHandle = CreateFile(PhysicalDrive,
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    NULL, OPEN_EXISTING, 0, 0);
  return g_MirrorDevHandle != INVALID_HANDLE_VALUE ? EXIT_SUCCESS : EXIT_FAILURE;
}

void MirrorDevTeardown()
{
  if (g_MirrorDevHandle != INVALID_HANDLE_VALUE)
  {
    CloseHandle(g_MirrorDevHandle);
  }
}

/**
* Rename this wmain to test read operations in isolation.
* TODO: Should replace this with an actual test case
*/
#if MIRROR_DEV_MAIN
int __cdecl wmain2(ULONG argc, PWCHAR argv[]) {
  g_DebugMode = TRUE;
  g_UseStdErr = TRUE;
  g_MirrorDevHandle = CreateFile(L"\\\\.\\PhysicalDrive1",
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    NULL, OPEN_EXISTING, 0, 0);

  if (g_MirrorDevHandle == INVALID_HANDLE_VALUE)
  {
    DbgPrint(L"CreateFile failed with error %d\n", GetLastError());
  }
  LONGLONG Offset = 528;
  DWORD offsetRemainder=0;
  DWORD ReadLengthDword;
  UCHAR Buffer[512];
  DWORD BufferLength = 512;
  DWORD ReadLength = BufferLength;

  if (MirrorDevSeekAligned(Offset, &offsetRemainder))
  {
    DbgPrint(L"Seek completed successfully to offset %u with remainder %u bytes\n", Offset, offsetRemainder);
    if (MirrorDevReadAligned(offsetRemainder, Buffer, ReadLength, &ReadLengthDword))
    {
      DbgPrint(L"Read %lu bytes from device, first first 4 bytes 0x%02x%02x%02x%02x\n\n",
        ReadLengthDword, ((UCHAR *)Buffer)[0], ((UCHAR *)Buffer)[1], ((UCHAR *)Buffer)[2], ((UCHAR *)Buffer)[3]);
    }
    else
    {
      DbgPrint(L"ReadFile failed with error %d\n", GetLastError());
    }
  }
  else
  {
    DbgPrint(L"Seek failed with error %d\n", GetLastError());
  }
  return 0;

}
#endif