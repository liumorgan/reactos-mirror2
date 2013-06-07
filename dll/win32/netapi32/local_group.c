/*
 * Copyright 2006 Robert Reif
 *
 * netapi32 local group functions
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "netapi32.h"

WINE_DEFAULT_DEBUG_CHANNEL(netapi32);


typedef enum _ENUM_PHASE
{
    BuiltinPhase,
    AccountPhase,
    DonePhase
} ENUM_PHASE;

typedef struct _ENUM_CONTEXT
{
    SAM_HANDLE ServerHandle;
    SAM_HANDLE DomainHandle;
    SAM_HANDLE BuiltinDomainHandle;
    SAM_HANDLE AccountDomainHandle;

    SAM_ENUMERATE_HANDLE EnumerationContext;
    PSAM_RID_ENUMERATION Buffer;
    ULONG Returned;
    ULONG Index;
    ENUM_PHASE Phase;

} ENUM_CONTEXT, *PENUM_CONTEXT;

typedef struct _MEMBER_ENUM_CONTEXT
{
    SAM_HANDLE ServerHandle;
    SAM_HANDLE DomainHandle;
    SAM_HANDLE AliasHandle;
    LSA_HANDLE LsaHandle;

    PSID *Sids;
    ULONG Count;
    PLSA_REFERENCED_DOMAIN_LIST Domains;
    PLSA_TRANSLATED_NAME Names;

} MEMBER_ENUM_CONTEXT, *PMEMBER_ENUM_CONTEXT;


static
NET_API_STATUS
BuildAliasInfoBuffer(PALIAS_GENERAL_INFORMATION AliasInfo,
                     DWORD level,
                     LPVOID *Buffer)
{
    LPVOID LocalBuffer = NULL;
    PLOCALGROUP_INFO_0 LocalInfo0;
    PLOCALGROUP_INFO_1 LocalInfo1;
    LPWSTR Ptr;
    ULONG Size = 0;
    NET_API_STATUS ApiStatus = NERR_Success;

    *Buffer = NULL;

    switch (level)
    {
        case 0:
            Size = sizeof(LOCALGROUP_INFO_0) +
                   AliasInfo->Name.Length + sizeof(WCHAR);
            break;

        case 1:
            Size = sizeof(LOCALGROUP_INFO_1) +
                   AliasInfo->Name.Length + sizeof(WCHAR) +
                   AliasInfo->AdminComment.Length + sizeof(WCHAR);
            break;

        default:
            ApiStatus = ERROR_INVALID_LEVEL;
            goto done;
    }

    ApiStatus = NetApiBufferAllocate(Size, &LocalBuffer);
    if (ApiStatus != NERR_Success)
        goto done;

    ZeroMemory(LocalBuffer, Size);

    switch (level)
    {
        case 0:
            LocalInfo0 = (PLOCALGROUP_INFO_0)LocalBuffer;

            Ptr = (LPWSTR)LocalInfo0++;
            LocalInfo0->lgrpi0_name = Ptr;

            memcpy(LocalInfo0->lgrpi0_name,
                   AliasInfo->Name.Buffer,
                   AliasInfo->Name.Length);
            LocalInfo0->lgrpi0_name[AliasInfo->Name.Length / sizeof(WCHAR)] = UNICODE_NULL;
            break;

        case 1:
            LocalInfo1 = (PLOCALGROUP_INFO_1)LocalBuffer;

            Ptr = (LPWSTR)((ULONG_PTR)LocalInfo1 + sizeof(LOCALGROUP_INFO_1));
            LocalInfo1->lgrpi1_name = Ptr;

            memcpy(LocalInfo1->lgrpi1_name,
                   AliasInfo->Name.Buffer,
                   AliasInfo->Name.Length);
            LocalInfo1->lgrpi1_name[AliasInfo->Name.Length / sizeof(WCHAR)] = UNICODE_NULL;

            Ptr = (LPWSTR)((ULONG_PTR)Ptr + AliasInfo->Name.Length + sizeof(WCHAR));
            LocalInfo1->lgrpi1_comment = Ptr;

            memcpy(LocalInfo1->lgrpi1_comment,
                   AliasInfo->AdminComment.Buffer,
                   AliasInfo->AdminComment.Length);
            LocalInfo1->lgrpi1_comment[AliasInfo->AdminComment.Length / sizeof(WCHAR)] = UNICODE_NULL;
            break;
    }

done:
    if (ApiStatus == NERR_Success)
    {
        *Buffer = LocalBuffer;
    }
    else
    {
        if (LocalBuffer != NULL)
            NetApiBufferFree(LocalBuffer);
    }

    return ApiStatus;
}


static
VOID
FreeAliasInfo(PALIAS_GENERAL_INFORMATION AliasInfo)
{
    if (AliasInfo->Name.Buffer != NULL)
        SamFreeMemory(AliasInfo->Name.Buffer);

    if (AliasInfo->AdminComment.Buffer != NULL)
        SamFreeMemory(AliasInfo->AdminComment.Buffer);

    SamFreeMemory(AliasInfo);
}


static
NET_API_STATUS
OpenAliasByName(SAM_HANDLE DomainHandle,
                PUNICODE_STRING AliasName,
                ULONG DesiredAccess,
                PSAM_HANDLE AliasHandle)
{
    PULONG RelativeIds = NULL;
    PSID_NAME_USE Use = NULL;
    NET_API_STATUS ApiStatus = NERR_Success;
    NTSTATUS Status = STATUS_SUCCESS;

    /* Get the RID for the given user name */
    Status = SamLookupNamesInDomain(DomainHandle,
                                    1,
                                    AliasName,
                                    &RelativeIds,
                                    &Use);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamLookupNamesInDomain failed (Status %08lx)\n", Status);
        return NetpNtStatusToApiStatus(Status);
    }

    /* Fail, if it is not an alias account */
    if (Use[0] != SidTypeAlias)
    {
        ERR("Object is not an Alias!\n");
        ApiStatus = NERR_GroupNotFound;
        goto done;
    }

    /* Open the alias account */
    Status = SamOpenAlias(DomainHandle,
                          DesiredAccess,
                          RelativeIds[0],
                          AliasHandle);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamOpenDomain failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

done:
    if (RelativeIds != NULL)
        SamFreeMemory(RelativeIds);

    if (Use != NULL)
        SamFreeMemory(Use);

    return ApiStatus;
}


/************************************************************
 * NetLocalGroupAdd  (NETAPI32.@)
 */
NET_API_STATUS
WINAPI
NetLocalGroupAdd(
    LPCWSTR servername,
    DWORD level,
    LPBYTE buf,
    LPDWORD parm_err)
{
    ALIAS_ADM_COMMENT_INFORMATION AdminComment;
    UNICODE_STRING ServerName;
    UNICODE_STRING AliasName;
    SAM_HANDLE ServerHandle = NULL;
    SAM_HANDLE DomainHandle = NULL;
    SAM_HANDLE AliasHandle = NULL;
    LPWSTR aliasname = NULL;
    LPWSTR aliascomment = NULL;
    ULONG RelativeId;
    NET_API_STATUS ApiStatus = NERR_Success;
    NTSTATUS Status = STATUS_SUCCESS;

    TRACE("(%s %d %p %p) stub!\n", debugstr_w(servername), level, buf,
          parm_err);

    /* Initialize the Server name*/
    if (servername != NULL)
        RtlInitUnicodeString(&ServerName, servername);

    /* Initialize the Alias name*/
    switch (level)
    {
        case 0:
            aliasname = ((PLOCALGROUP_INFO_0)buf)->lgrpi0_name;
            aliascomment = NULL;
            break;

        case 1:
            aliasname = ((PLOCALGROUP_INFO_1)buf)->lgrpi1_name;
            aliascomment = ((PLOCALGROUP_INFO_1)buf)->lgrpi1_comment;
            break;

        default:
            return ERROR_INVALID_LEVEL;
    }

    RtlInitUnicodeString(&AliasName, aliasname);

    /* Connect to the SAM Server */
    Status = SamConnect((servername != NULL) ? &ServerName : NULL,
                        &ServerHandle,
                        SAM_SERVER_CONNECT | SAM_SERVER_LOOKUP_DOMAIN,
                        NULL);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamConnect failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    /* Open the Builtin Domain */
    Status = OpenBuiltinDomain(ServerHandle,
                               DOMAIN_LOOKUP,
                               &DomainHandle);
    if (!NT_SUCCESS(Status))
    {
        ERR("OpenBuiltinDomain failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    /* Try to open the Alias Account in the Builtin Domain */
    ApiStatus = OpenAliasByName(DomainHandle,
                                &AliasName,
                                ALIAS_READ_INFORMATION,
                                &AliasHandle);
    if (ApiStatus == NERR_Success)
    {
        ERR("OpenAliasByName: alias %wZ already exists!\n", &AliasName);

        SamCloseHandle(AliasHandle);
        ApiStatus = ERROR_ALIAS_EXISTS;
        goto done;
    }

    ApiStatus = NERR_Success;

    /* Close the Builtin Domain */
    SamCloseHandle(DomainHandle);
    DomainHandle = NULL;

    /* Open the account domain */
    Status = OpenAccountDomain(ServerHandle,
                               (servername != NULL) ? &ServerName : NULL,
                               DOMAIN_CREATE_ALIAS | DOMAIN_LOOKUP,
                               &DomainHandle);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamOpenDomain failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    /* Create the alias */
    Status = SamCreateAliasInDomain(DomainHandle,
                                    &AliasName,
                                    DELETE | ALIAS_WRITE_ACCOUNT,
                                    &AliasHandle,
                                    &RelativeId);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamCreateAliasInDomain failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    TRACE("Created alias \"%wZ\" (RID: %lu)\n", &AliasName, RelativeId);

    /* Set the admin comment */
    if (level == 1)
    {
        RtlInitUnicodeString(&AdminComment.AdminComment, aliascomment);

        Status = SamSetInformationAlias(AliasHandle,
                                        AliasAdminCommentInformation,
                                        &AdminComment);
        if (!NT_SUCCESS(Status))
        {
            ERR("SamSetInformationAlias failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);

            /* Delete the Alias if the Comment could not be set */
            SamDeleteAlias(AliasHandle);

            goto done;
        }
    }

done:
    if (AliasHandle != NULL)
        SamCloseHandle(AliasHandle);

    if (DomainHandle != NULL)
        SamCloseHandle(DomainHandle);

    if (ServerHandle != NULL)
        SamCloseHandle(ServerHandle);

    return ApiStatus;
}


/************************************************************
 *                NetLocalGroupAddMember  (NETAPI32.@)
 */
NET_API_STATUS WINAPI NetLocalGroupAddMember(
    LPCWSTR servername,
    LPCWSTR groupname,
    PSID membersid)
{
    FIXME("(%s %s %p) stub!\n", debugstr_w(servername),
          debugstr_w(groupname), membersid);
    return NERR_Success;
}

/************************************************************
 *                NetLocalGroupAddMembers  (NETAPI32.@)
 */
NET_API_STATUS WINAPI NetLocalGroupAddMembers(
    LPCWSTR servername,
    LPCWSTR groupname,
    DWORD level,
    LPBYTE buf,
    DWORD totalentries)
{
    FIXME("(%s %s %d %p %d) stub!\n", debugstr_w(servername),
          debugstr_w(groupname), level, buf, totalentries);
    return NERR_Success;
}


/************************************************************
 *                NetLocalGroupDel  (NETAPI32.@)
 */
NET_API_STATUS
WINAPI
NetLocalGroupDel(
    LPCWSTR servername,
    LPCWSTR groupname)
{
    UNICODE_STRING ServerName;
    UNICODE_STRING GroupName;
    SAM_HANDLE ServerHandle = NULL;
    SAM_HANDLE DomainHandle = NULL;
    SAM_HANDLE AliasHandle = NULL;
    NET_API_STATUS ApiStatus = NERR_Success;
    NTSTATUS Status = STATUS_SUCCESS;

    TRACE("(%s %s)\n", debugstr_w(servername), debugstr_w(groupname));

    if (servername != NULL)
        RtlInitUnicodeString(&ServerName, servername);

    RtlInitUnicodeString(&GroupName, groupname);

    /* Connect to the SAM Server */
    Status = SamConnect((servername != NULL) ? &ServerName : NULL,
                        &ServerHandle,
                        SAM_SERVER_CONNECT | SAM_SERVER_LOOKUP_DOMAIN,
                        NULL);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamConnect failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    /* Open the Builtin Domain */
    Status = OpenBuiltinDomain(ServerHandle,
                               DOMAIN_LOOKUP,
                               &DomainHandle);
    if (!NT_SUCCESS(Status))
    {
        ERR("OpenBuiltinDomain failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    /* Open the alias account in the builtin domain */
    ApiStatus = OpenAliasByName(DomainHandle,
                                &GroupName,
                                DELETE,
                                &AliasHandle);
    if (ApiStatus != NERR_Success && ApiStatus != ERROR_NONE_MAPPED)
    {
        TRACE("OpenAliasByName failed (ApiStatus %lu)\n", ApiStatus);
        goto done;
    }

    if (AliasHandle == NULL)
    {
        if (DomainHandle != NULL)
        {
            SamCloseHandle(DomainHandle);
            DomainHandle = NULL;
        }

        /* Open the Acount Domain */
        Status = OpenAccountDomain(ServerHandle,
                                   (servername != NULL) ? &ServerName : NULL,
                                   DOMAIN_LOOKUP,
                                   &DomainHandle);
        if (!NT_SUCCESS(Status))
        {
            ERR("OpenAccountDomain failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        /* Open the alias account in the account domain */
        ApiStatus = OpenAliasByName(DomainHandle,
                                    &GroupName,
                                    DELETE,
                                    &AliasHandle);
        if (ApiStatus != NERR_Success)
        {
            ERR("OpenAliasByName failed (ApiStatus %lu)\n", ApiStatus);
            if (ApiStatus == ERROR_NONE_MAPPED)
                ApiStatus = NERR_GroupNotFound;
            goto done;
        }
    }

    /* Delete the alias */
    Status = SamDeleteAlias(AliasHandle);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamDeleteAlias failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

done:
    if (AliasHandle != NULL)
        SamCloseHandle(AliasHandle);

    if (DomainHandle != NULL)
        SamCloseHandle(DomainHandle);

    if (ServerHandle != NULL)
        SamCloseHandle(ServerHandle);

    return ApiStatus;
}


/************************************************************
 *                NetLocalGroupDelMember  (NETAPI32.@)
 */
NET_API_STATUS WINAPI NetLocalGroupDelMember(
    LPCWSTR servername,
    LPCWSTR groupname,
    PSID membersid)
{
    FIXME("(%s %s %p) stub!\n", debugstr_w(servername),
          debugstr_w(groupname), membersid);
    return NERR_Success;
}

/************************************************************
 *                NetLocalGroupDelMembers  (NETAPI32.@)
 */
NET_API_STATUS WINAPI NetLocalGroupDelMembers(
    LPCWSTR servername,
    LPCWSTR groupname,
    DWORD level,
    LPBYTE buf,
    DWORD totalentries)
{
    FIXME("(%s %s %d %p %d) stub!\n", debugstr_w(servername),
          debugstr_w(groupname), level, buf, totalentries);
    return NERR_Success;
}


/************************************************************
 *                NetLocalGroupEnum  (NETAPI32.@)
 */
NET_API_STATUS
WINAPI
NetLocalGroupEnum(
    LPCWSTR servername,
    DWORD level,
    LPBYTE* bufptr,
    DWORD prefmaxlen,
    LPDWORD entriesread,
    LPDWORD totalentries,
    PDWORD_PTR resumehandle)
{
    UNICODE_STRING ServerName;
    PSAM_RID_ENUMERATION CurrentAlias;
    PENUM_CONTEXT EnumContext = NULL;
    ULONG i;
    SAM_HANDLE AliasHandle = NULL;
    PALIAS_GENERAL_INFORMATION AliasInfo = NULL;
    LPVOID Buffer = NULL;
    NET_API_STATUS ApiStatus = NERR_Success;
    NTSTATUS Status = STATUS_SUCCESS;

    TRACE("(%s %d %p %d %p %p %p) stub!\n", debugstr_w(servername),
          level, bufptr, prefmaxlen, entriesread, totalentries, resumehandle);

    *entriesread = 0;
    *totalentries = 0;
    *bufptr = NULL;

    if (servername != NULL)
        RtlInitUnicodeString(&ServerName, servername);

    if (resumehandle != NULL && *resumehandle != 0)
    {
        EnumContext = (PENUM_CONTEXT)*resumehandle;
    }
    else
    {
        ApiStatus = NetApiBufferAllocate(sizeof(ENUM_CONTEXT), (PVOID*)&EnumContext);
        if (ApiStatus != NERR_Success)
            goto done;

        EnumContext->EnumerationContext = 0;
        EnumContext->Buffer = NULL;
        EnumContext->Returned = 0;
        EnumContext->Index = 0;

        Status = SamConnect((servername != NULL) ? &ServerName : NULL,
                            &EnumContext->ServerHandle,
                            SAM_SERVER_CONNECT | SAM_SERVER_LOOKUP_DOMAIN,
                            NULL);
        if (!NT_SUCCESS(Status))
        {
            ERR("SamConnect failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        Status = OpenAccountDomain(EnumContext->ServerHandle,
                                   (servername != NULL) ? &ServerName : NULL,
                                   DOMAIN_LIST_ACCOUNTS | DOMAIN_LOOKUP,
                                   &EnumContext->AccountDomainHandle);
        if (!NT_SUCCESS(Status))
        {
            ERR("OpenAccountDomain failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        Status = OpenBuiltinDomain(EnumContext->ServerHandle,
                                   DOMAIN_LIST_ACCOUNTS | DOMAIN_LOOKUP,
                                   &EnumContext->BuiltinDomainHandle);
        if (!NT_SUCCESS(Status))
        {
            ERR("OpenBuiltinDomain failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        EnumContext->Phase = BuiltinPhase;
        EnumContext->DomainHandle = EnumContext->BuiltinDomainHandle;
    }


//    while (TRUE)
//    {
        TRACE("EnumContext->Index: %lu\n", EnumContext->Index);
        TRACE("EnumContext->Returned: %lu\n", EnumContext->Returned);

        if (EnumContext->Index >= EnumContext->Returned)
        {
            TRACE("Calling SamEnumerateAliasesInDomain\n");

            Status = SamEnumerateAliasesInDomain(EnumContext->DomainHandle,
                                                 &EnumContext->EnumerationContext,
                                                 (PVOID *)&EnumContext->Buffer,
                                                 prefmaxlen,
                                                 &EnumContext->Returned);

            TRACE("SamEnumerateAliasesInDomain returned (Status %08lx)\n", Status);
            if (!NT_SUCCESS(Status))
            {
                ERR("SamEnumerateAliasesInDomain failed (Status %08lx)\n", Status);
                ApiStatus = NetpNtStatusToApiStatus(Status);
                goto done;
            }

            if (Status == STATUS_MORE_ENTRIES)
            {
                ApiStatus = NERR_BufTooSmall;
                goto done;
            }
        }

        TRACE("EnumContext: %lu\n", EnumContext);
        TRACE("EnumContext->Returned: %lu\n", EnumContext->Returned);
        TRACE("EnumContext->Buffer: %p\n", EnumContext->Buffer);

        /* Get a pointer to the current alias */
        CurrentAlias = &EnumContext->Buffer[EnumContext->Index];

        TRACE("RID: %lu\n", CurrentAlias->RelativeId);

        Status = SamOpenAlias(EnumContext->DomainHandle,
                              ALIAS_READ_INFORMATION,
                              CurrentAlias->RelativeId,
                              &AliasHandle);
        if (!NT_SUCCESS(Status))
        {
            ERR("SamOpenAlias failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        Status = SamQueryInformationAlias(AliasHandle,
                                          AliasGeneralInformation,
                                          (PVOID *)&AliasInfo);
        if (!NT_SUCCESS(Status))
        {
            ERR("SamQueryInformationAlias failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        SamCloseHandle(AliasHandle);
        AliasHandle = NULL;

        TRACE("Name: %S\n", AliasInfo->Name.Buffer);
        TRACE("Comment: %S\n", AliasInfo->AdminComment.Buffer);

        ApiStatus = BuildAliasInfoBuffer(AliasInfo,
                                         level,
                                         &Buffer);
        if (ApiStatus != NERR_Success)
            goto done;

        if (AliasInfo != NULL)
        {
            FreeAliasInfo(AliasInfo);
            AliasInfo = NULL;
        }

        EnumContext->Index++;

        (*entriesread)++;

        if (EnumContext->Index == EnumContext->Returned)
        {
            switch (EnumContext->Phase)
            {
                case BuiltinPhase:
                    EnumContext->Phase = AccountPhase;
                    EnumContext->DomainHandle = EnumContext->AccountDomainHandle;
                    EnumContext->EnumerationContext = 0;
                    EnumContext->Index = 0;
                    EnumContext->Returned = 0;

                    if (EnumContext->Buffer != NULL)
                    {
                        for (i = 0; i < EnumContext->Returned; i++)
                        {
                            SamFreeMemory(EnumContext->Buffer[i].Name.Buffer);
                        }

                        SamFreeMemory(EnumContext->Buffer);
                        EnumContext->Buffer = NULL;
                    }
                    break;

                case AccountPhase:
                case DonePhase:
                    EnumContext->Phase = DonePhase;
                    break;
            }
        }
//    }

done:
    if (ApiStatus == NERR_Success && EnumContext->Phase != DonePhase)
        ApiStatus = ERROR_MORE_DATA;

    if (EnumContext != NULL)
        *totalentries = EnumContext->Returned;

    if (resumehandle == NULL || ApiStatus != ERROR_MORE_DATA)
    {
        if (EnumContext != NULL)
        {
            if (EnumContext->BuiltinDomainHandle != NULL)
                SamCloseHandle(EnumContext->BuiltinDomainHandle);

            if (EnumContext->AccountDomainHandle != NULL)
                SamCloseHandle(EnumContext->AccountDomainHandle);

            if (EnumContext->ServerHandle != NULL)
                SamCloseHandle(EnumContext->ServerHandle);

            if (EnumContext->Buffer != NULL)
            {
                for (i = 0; i < EnumContext->Returned; i++)
                {
                    SamFreeMemory(EnumContext->Buffer[i].Name.Buffer);
                }

                SamFreeMemory(EnumContext->Buffer);
            }

            NetApiBufferFree(EnumContext);
            EnumContext = NULL;
        }
    }

    if (AliasHandle != NULL)
        SamCloseHandle(AliasHandle);

    if (AliasInfo != NULL)
        FreeAliasInfo(AliasInfo);

    if (resumehandle != NULL)
        *resumehandle = (DWORD_PTR)EnumContext;

    *bufptr = (LPBYTE)Buffer;

    TRACE ("return %lu\n", ApiStatus);

    return ApiStatus;
}


/************************************************************
 * NetLocalGroupGetInfo  (NETAPI32.@)
 */
NET_API_STATUS
WINAPI
NetLocalGroupGetInfo(
    LPCWSTR servername,
    LPCWSTR groupname,
    DWORD level,
    LPBYTE* bufptr)
{
    UNICODE_STRING ServerName;
    UNICODE_STRING GroupName;
    SAM_HANDLE ServerHandle = NULL;
    SAM_HANDLE DomainHandle = NULL;
    SAM_HANDLE AliasHandle = NULL;
    PALIAS_GENERAL_INFORMATION AliasInfo = NULL;
    LPVOID Buffer = NULL;
    NET_API_STATUS ApiStatus = NERR_Success;
    NTSTATUS Status = STATUS_SUCCESS;

    TRACE("(%s %s %d %p) stub!\n", debugstr_w(servername),
          debugstr_w(groupname), level, bufptr);

    if (servername != NULL)
        RtlInitUnicodeString(&ServerName, servername);

    RtlInitUnicodeString(&GroupName, groupname);

    /* Connect to the SAM Server */
    Status = SamConnect((servername != NULL) ? &ServerName : NULL,
                        &ServerHandle,
                        SAM_SERVER_CONNECT | SAM_SERVER_LOOKUP_DOMAIN,
                        NULL);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamConnect failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    /* Open the Builtin Domain */
    Status = OpenBuiltinDomain(ServerHandle,
                               DOMAIN_LOOKUP,
                               &DomainHandle);
    if (!NT_SUCCESS(Status))
    {
        ERR("OpenBuiltinDomain failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    /* Open the alias account in the builtin domain */
    ApiStatus = OpenAliasByName(DomainHandle,
                                &GroupName,
                                ALIAS_READ_INFORMATION,
                                &AliasHandle);
    if (ApiStatus != NERR_Success && ApiStatus != ERROR_NONE_MAPPED)
    {
        ERR("OpenAliasByName failed (ApiStatus %lu)\n", ApiStatus);
        goto done;
    }

    if (AliasHandle == NULL)
    {
        if (DomainHandle != NULL)
            SamCloseHandle(DomainHandle);

        /* Open the Acount Domain */
        Status = OpenAccountDomain(ServerHandle,
                                   (servername != NULL) ? &ServerName : NULL,
                                   DOMAIN_LOOKUP,
                                   &DomainHandle);
        if (!NT_SUCCESS(Status))
        {
            ERR("OpenAccountDomain failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        /* Open the alias account in the account domain */
        ApiStatus = OpenAliasByName(DomainHandle,
                                    &GroupName,
                                    ALIAS_READ_INFORMATION,
                                    &AliasHandle);
        if (ApiStatus != NERR_Success)
        {
            ERR("OpenAliasByName failed (ApiStatus %lu)\n", ApiStatus);
            if (ApiStatus == ERROR_NONE_MAPPED)
                ApiStatus = NERR_GroupNotFound;
            goto done;
        }
    }

    Status = SamQueryInformationAlias(AliasHandle,
                                      AliasGeneralInformation,
                                      (PVOID *)&AliasInfo);
    if (!NT_SUCCESS(Status))
    {
        ERR("SamQueryInformationAlias failed (Status %08lx)\n", Status);
        ApiStatus = NetpNtStatusToApiStatus(Status);
        goto done;
    }

    ApiStatus = BuildAliasInfoBuffer(AliasInfo,
                                     level,
                                     &Buffer);
    if (ApiStatus != NERR_Success)
        goto done;

done:
    if (AliasInfo != NULL)
        FreeAliasInfo(AliasInfo);

    if (AliasHandle != NULL)
        SamCloseHandle(AliasHandle);

    if (DomainHandle != NULL)
        SamCloseHandle(DomainHandle);

    if (ServerHandle != NULL)
        SamCloseHandle(ServerHandle);

    *bufptr = (LPBYTE)Buffer;

    return ApiStatus;
}


/************************************************************
 *                NetLocalGroupGetMembers  (NETAPI32.@)
 */
NET_API_STATUS
WINAPI
NetLocalGroupGetMembers(
    LPCWSTR servername,
    LPCWSTR localgroupname,
    DWORD level,
    LPBYTE* bufptr,
    DWORD prefmaxlen,
    LPDWORD entriesread,
    LPDWORD totalentries,
    PDWORD_PTR resumehandle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING ServerName;
    UNICODE_STRING AliasName;
    PMEMBER_ENUM_CONTEXT EnumContext = NULL;
    LPVOID Buffer = NULL;
    PLOCALGROUP_MEMBERS_INFO_0 MembersInfo0;
    PLOCALGROUP_MEMBERS_INFO_1 MembersInfo1;
    LPWSTR Ptr;
    ULONG Size = 0;
    ULONG SidLength;
    ULONG i;
    NET_API_STATUS ApiStatus = NERR_Success;
    NTSTATUS Status = STATUS_SUCCESS;

    TRACE("(%s %s %d %p %d, %p %p %p)\n", debugstr_w(servername),
          debugstr_w(localgroupname), level, bufptr, prefmaxlen, entriesread,
          totalentries, resumehandle);

    *entriesread = 0;
    *totalentries = 0;
    *bufptr = NULL;

    if (servername != NULL)
        RtlInitUnicodeString(&ServerName, servername);

    RtlInitUnicodeString(&AliasName, localgroupname);

    if (resumehandle != NULL && *resumehandle != 0)
    {
        EnumContext = (PMEMBER_ENUM_CONTEXT)*resumehandle;
    }
    else
    {
        /* Allocate the enumeration context */
        ApiStatus = NetApiBufferAllocate(sizeof(MEMBER_ENUM_CONTEXT), (PVOID*)&EnumContext);
        if (ApiStatus != NERR_Success)
            goto done;

        /* Connect to the SAM Server */
        Status = SamConnect((servername != NULL) ? &ServerName : NULL,
                            &EnumContext->ServerHandle,
                            SAM_SERVER_CONNECT | SAM_SERVER_LOOKUP_DOMAIN,
                            NULL);
        if (!NT_SUCCESS(Status))
        {
            ERR("SamConnect failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        /* Open the Builtin Domain */
        Status = OpenBuiltinDomain(EnumContext->ServerHandle,
                                   DOMAIN_LOOKUP,
                                   &EnumContext->DomainHandle);
        if (!NT_SUCCESS(Status))
        {
            ERR("OpenBuiltinDomain failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        /* Open the alias account in the builtin domain */
        ApiStatus = OpenAliasByName(EnumContext->DomainHandle,
                                    &AliasName,
                                    ALIAS_LIST_MEMBERS,
                                    &EnumContext->AliasHandle);
        if (ApiStatus != NERR_Success && ApiStatus != ERROR_NONE_MAPPED)
        {
            ERR("OpenAliasByName failed (ApiStatus %lu)\n", ApiStatus);
            goto done;
        }

        if (EnumContext->AliasHandle == NULL)
        {
            if (EnumContext->DomainHandle != NULL)
                SamCloseHandle(EnumContext->DomainHandle);

            /* Open the Acount Domain */
            Status = OpenAccountDomain(EnumContext->ServerHandle,
                                       (servername != NULL) ? &ServerName : NULL,
                                       DOMAIN_LOOKUP,
                                       &EnumContext->DomainHandle);
            if (!NT_SUCCESS(Status))
            {
                ERR("OpenAccountDomain failed (Status %08lx)\n", Status);
                ApiStatus = NetpNtStatusToApiStatus(Status);
                goto done;
            }

            /* Open the alias account in the account domain */
            ApiStatus = OpenAliasByName(EnumContext->DomainHandle,
                                        &AliasName,
                                        ALIAS_LIST_MEMBERS,
                                        &EnumContext->AliasHandle);
            if (ApiStatus != NERR_Success)
            {
                ERR("OpenAliasByName failed (ApiStatus %lu)\n", ApiStatus);
                if (ApiStatus == ERROR_NONE_MAPPED)
                    ApiStatus = NERR_GroupNotFound;
                goto done;
            }
        }

        /* Get the member list */
        Status = SamGetMembersInAlias(EnumContext->AliasHandle,
                                      &EnumContext->Sids,
                                      &EnumContext->Count);
        if (!NT_SUCCESS(Status))
        {
            ERR("SamGetMemberInAlias failed (Status %08lx)\n", Status);
            ApiStatus = NetpNtStatusToApiStatus(Status);
            goto done;
        }

        if (EnumContext->Count == 0)
        {
            TRACE("No member found. We're done.\n");
            ApiStatus = NERR_Success;
            goto done;
        }


        if (level != 0)
        {
            InitializeObjectAttributes(&ObjectAttributes,
                                       NULL,
                                       0,
                                       0,
                                       NULL);

            Status = LsaOpenPolicy((servername != NULL) ? &ServerName : NULL,
                                   (PLSA_OBJECT_ATTRIBUTES)&ObjectAttributes,
                                   POLICY_EXECUTE,
                                   &EnumContext->LsaHandle);
            if (!NT_SUCCESS(Status))
            {
                ApiStatus = NetpNtStatusToApiStatus(Status);
                goto done;
            }

            Status = LsaLookupSids(EnumContext->LsaHandle,
                                   EnumContext->Count,
                                   EnumContext->Sids,
                                   &EnumContext->Domains,
                                   &EnumContext->Names);
            if (!NT_SUCCESS(Status))
            {
                ApiStatus = NetpNtStatusToApiStatus(Status);
                goto done;
            }
        }
    }

    /* Calculate the required buffer size */
    for (i = 0; i < EnumContext->Count; i++)
    {
        switch (level)
        {
            case 0:
                Size = sizeof(LOCALGROUP_MEMBERS_INFO_0) +
                       RtlLengthSid(EnumContext->Sids[i]);
                break;

            case 1:
                Size = sizeof(LOCALGROUP_MEMBERS_INFO_1) +
                       RtlLengthSid(EnumContext->Sids[i]) +
                       EnumContext->Names[i].Name.Length + sizeof(WCHAR);
                break;

            default:
                ApiStatus = ERROR_INVALID_LEVEL;
                goto done;
        }
    }

    /* Allocate the member buffer */
    ApiStatus = NetApiBufferAllocate(Size, &Buffer);
    if (ApiStatus != NERR_Success)
        goto done;

    ZeroMemory(Buffer, Size);

    /* Fill the member buffer */
    switch (level)
    {
        case 0:
            MembersInfo0 = (PLOCALGROUP_MEMBERS_INFO_0)Buffer;
            Ptr = (PVOID)((ULONG_PTR)Buffer + sizeof(LOCALGROUP_MEMBERS_INFO_0) * EnumContext->Count);
            break;

        case 1:
            MembersInfo1 = (PLOCALGROUP_MEMBERS_INFO_1)Buffer;
            Ptr = (PVOID)((ULONG_PTR)Buffer + sizeof(LOCALGROUP_MEMBERS_INFO_1) * EnumContext->Count);
            break;
    }

    for (i = 0; i < EnumContext->Count; i++)
    {
        switch (level)
        {
            case 0:
                MembersInfo0->lgrmi0_sid = (PSID)Ptr;

                SidLength = RtlLengthSid(EnumContext->Sids[i]);
                memcpy(MembersInfo0->lgrmi0_sid,
                       EnumContext->Sids[i],
                       SidLength);
                Ptr = (PVOID)((ULONG_PTR)Ptr + SidLength);
                break;

            case 1:
                MembersInfo1->lgrmi1_sid = (PSID)Ptr;

                SidLength = RtlLengthSid(EnumContext->Sids[i]);
                memcpy(MembersInfo1->lgrmi1_sid,
                       EnumContext->Sids[i],
                       SidLength);

                Ptr = (PVOID)((ULONG_PTR)Ptr + SidLength);

                MembersInfo1->lgrmi1_sidusage = EnumContext->Names[i].Use;

                TRACE("Name: %S\n", EnumContext->Names[i].Name.Buffer);

                MembersInfo1->lgrmi1_name = (LPWSTR)Ptr;

                memcpy(MembersInfo1->lgrmi1_name,
                       EnumContext->Names[i].Name.Buffer,
                       EnumContext->Names[i].Name.Length);
                break;
        }
    }

    *entriesread = EnumContext->Count;

    *bufptr = (LPBYTE)Buffer;

done:
    if (EnumContext != NULL)
        *totalentries = EnumContext->Count;

    if (resumehandle == NULL || ApiStatus != ERROR_MORE_DATA)
    {
        /* Release the enumeration context */
        if (EnumContext != NULL)
        {
            if (EnumContext->LsaHandle != NULL)
                LsaClose(EnumContext->LsaHandle);

            if (EnumContext->AliasHandle != NULL)
                SamCloseHandle(EnumContext->AliasHandle);

            if (EnumContext->DomainHandle != NULL)
                SamCloseHandle(EnumContext->DomainHandle);

            if (EnumContext->ServerHandle != NULL)
                SamCloseHandle(EnumContext->ServerHandle);

            if (EnumContext->Sids != NULL)
                SamFreeMemory(EnumContext->Sids);

            if (EnumContext->Domains != NULL)
                LsaFreeMemory(EnumContext->Domains);

            if (EnumContext->Names != NULL)
                LsaFreeMemory(EnumContext->Names);

            NetApiBufferFree(EnumContext);
            EnumContext = NULL;
        }
    }

    return ApiStatus;
}


/************************************************************
 *                NetLocalGroupSetInfo  (NETAPI32.@)
 */
NET_API_STATUS WINAPI NetLocalGroupSetInfo(
    LPCWSTR servername,
    LPCWSTR groupname,
    DWORD level,
    LPBYTE buf,
    LPDWORD parm_err)
{
    FIXME("(%s %s %d %p %p) stub!\n", debugstr_w(servername),
          debugstr_w(groupname), level, buf, parm_err);
    return NERR_Success;
}

/************************************************************
 *                NetLocalGroupSetMember (NETAPI32.@)
 */
NET_API_STATUS WINAPI NetLocalGroupSetMembers(
    LPCWSTR servername,
    LPCWSTR groupname,
    DWORD level,
    LPBYTE buf,
    DWORD totalentries)
{
    FIXME("(%s %s %d %p %d) stub!\n", debugstr_w(servername),
            debugstr_w(groupname), level, buf, totalentries);
    return NERR_Success;
}

/* EOF */
