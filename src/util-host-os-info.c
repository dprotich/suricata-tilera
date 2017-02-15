/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * Host info utility functions
 */

#include "suricata-common.h"
#include "util-host-os-info.h"
#include "util-error.h"
#include "util-debug.h"
#include "util-radix-tree.h"
#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"

#include "conf.h"
#include "conf-yaml-loader.h"

#include "util-enum.h"
#include "util-unittest.h"

/** Enum map for the various OS flavours */
SCEnumCharMap sc_hinfo_os_policy_map[ ] = {
    { "none",        OS_POLICY_NONE },
    { "bsd",         OS_POLICY_BSD },
    { "bsd-right",   OS_POLICY_BSD_RIGHT },
    { "old-linux",   OS_POLICY_OLD_LINUX },
    { "linux",       OS_POLICY_LINUX },
    { "old-solaris", OS_POLICY_OLD_SOLARIS },
    { "solaris",     OS_POLICY_SOLARIS },
    { "hpux10",      OS_POLICY_HPUX10 },
    { "hpux11",      OS_POLICY_HPUX11 },
    { "irix",        OS_POLICY_IRIX },
    { "macos",       OS_POLICY_MACOS },
    { "windows",     OS_POLICY_WINDOWS },
    { "vista",       OS_POLICY_VISTA },
    { "windows2k3",  OS_POLICY_WINDOWS2K3 },
    { NULL,          -1 },
};

/** Radix tree that holds the host OS information */
static SCRadixTree *sc_hinfo_tree = NULL;

/**
 * \brief Validates an IPV4 address and returns the network endian arranged
 *        version of the IPV4 address
 *
 * \param addr Pointer to a character string containing an IPV4 address.  A
 *             valid IPV4 address is a character string containing a dotted
 *             format of "ddd.ddd.ddd.ddd"
 *
 * \retval Pointer to an in_addr instance containing the network endian format
 *         of the IPV4 address
 * \retval NULL if the IPV4 address is invalid
 */
static struct in_addr *SCHInfoValidateIPV4Address(const char *addr_str)
{
    struct in_addr *addr = NULL;

    if ( (addr = SCMalloc(sizeof(struct in_addr))) == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Fatal error encountered in SCHInfoValidateIPV4Address. Mem not allocated");
        return NULL;
    }

    if (inet_pton(AF_INET, addr_str, addr) <= 0) {
        SCFree(addr);
        return NULL;
    }

    return addr;
}

/**
 * \brief Validates an IPV6 address and returns the network endian arranged
 *        version of the IPV6 addresss
 *
 * \param addr Pointer to a character string containing an IPV6 address
 *
 * \retval Pointer to a in6_addr instance containing the network endian format
 *         of the IPV6 address
 * \retval NULL if the IPV6 address is invalid
 */
static struct in6_addr *SCHInfoValidateIPV6Address(const char *addr_str)
{
    struct in6_addr *addr = NULL;

    if ( (addr = SCMalloc(sizeof(struct in6_addr))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCHInfoValidateIPV6Address. Exiting...");
        exit(EXIT_FAILURE);
    }

    if (inet_pton(AF_INET6, addr_str, addr) <= 0) {
        SCFree(addr);
        return NULL;
    }

    return addr;
}

/**
 * \brief Allocates the host_os flavour wrapped in user_data variable to be sent
 *        along with the key to the radix tree
 *
 * \param host_os Pointer to a character string containing the host_os flavour
 *
 * \retval user_data On success, pointer to the user_data that has to be sent
 *                   along with the key, to be added to the Radix tree; NULL on
 *                   failure
 * \initonly
 */
static void *SCHInfoAllocUserDataOSPolicy(const char *host_os)
{
    int *user_data = NULL;

    if ( (user_data = SCMalloc(sizeof(int))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Error allocating memory. Exiting");
        exit(EXIT_FAILURE);
    }

    /* the host os flavour that has to be sent as user data */
    if ( (*user_data = SCMapEnumNameToValue(host_os, sc_hinfo_os_policy_map)) == -1) {
        SCLogError(SC_ERR_INVALID_ENUM_MAP, "Invalid enum map inside "
                   "SCHInfoAddHostOSInfo()");
        SCFree(user_data);
        return NULL;
    }

    return (void *)user_data;
}

/**
 * \brief Used to free the user data that is allocated by host_os_info API
 *
 * \param Pointer to the data that has to be freed
 */
static void SCHInfoFreeUserDataOSPolicy(void *data)
{
    if (data != NULL)
        SCFree(data);

    return;
}

/**
 * \brief Culls the non-netmask portion of the ip address.
 *
 *        This function can also be used for any general purpose use, to mask
 *        the first netmask bits of the stream data sent as argument
 *
 * \param stream  Pointer to the data to be masked
 * \param netmask The mask length(netmask)
 * \param bitlen  The bitlen of the stream
 */
static void SCHInfoMaskIPNetblock(uint8_t *stream, int netmask, int bitlen)
{
    int bytes = 0;
    int mask = 0;
    int i = 0;

    bytes = bitlen / 8;
    for (i = 0; i < bytes; i++) {
        mask = -1;
        if ( ((i + 1) * 8) > netmask) {
            if ( ((i + 1) * 8 - netmask) < 8)
                mask = -1 << ((i + 1) * 8 - netmask);
            else
                mask = 0;
        }
        stream[i] &= mask;
    }

    return;
}

/**
 * \brief Used to add the host-os-info data obtained from the conf
 *
 * \param host_os          The host_os name/flavour from the conf file
 * \param host_os_ip_range Pointer to a char string holding the ip/ip_netblock
 *                         for the host_os specified in the first argument
 * \param is_ipv4          Indicates if the ip address to be considered for the
 *                         default configuration is IPV4; if not it is IPV6.
 *                         Specified using SC_HINFO_IS_IPV6 or SC_HINFO_IS_IPV4
 *
 * \retval  0 On successfully adding the host os info to the Radix tree
 * \retval -1 On failure
 * \initonly (only specified from config, at the startup)
 */
int SCHInfoAddHostOSInfo(char *host_os, char *host_os_ip_range, int is_ipv4)
{
    char *ip_str = NULL;
    char *ip_str_rem = NULL;
    struct in_addr *ipv4_addr = NULL;
    struct in6_addr *ipv6_addr = NULL;
    char *netmask_str = NULL;
    int netmask_value = 0;
    int *user_data = NULL;
    char recursive = FALSE;

    if (host_os == NULL || host_os_ip_range == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid arguments");
        return -1;
    }

    /* create the radix tree that would hold all the host os info */
    if (sc_hinfo_tree == NULL)
        sc_hinfo_tree = SCRadixCreateRadixTree(SCHInfoFreeUserDataOSPolicy, NULL);

    /* the host os flavour that has to be sent as user data */
    if ( (user_data = SCHInfoAllocUserDataOSPolicy(host_os)) == NULL) {
        SCLogError(SC_ERR_INVALID_ENUM_MAP, "Invalid enum map inside");
        return -1;
    }

    /* if we have a default configuration set the appropriate values for the
     * netblocks */
    if ( (strcasecmp(host_os_ip_range, "default")) == 0) {
        if (is_ipv4)
            host_os_ip_range = "0.0.0.0/0";
        else
            host_os_ip_range = "::/0";
    }

    if ( (ip_str = SCStrdup(host_os_ip_range)) == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }

    /* check if we have more addresses in the host_os_ip_range */
    if ((ip_str_rem = index(ip_str, ',')) != NULL) {
        ip_str_rem[0] = '\0';
        ip_str_rem++;
        recursive = TRUE;
    }

    /* check if we have received a netblock */
    if ( (netmask_str = index(ip_str, '/')) != NULL) {
        netmask_str[0] = '\0';
        netmask_str++;
    }

    if (index(ip_str, ':') == NULL) {
        /* if we are here, we have an IPV4 address */
        if ( (ipv4_addr = SCHInfoValidateIPV4Address(ip_str)) == NULL) {
            SCLogError(SC_ERR_INVALID_IPV4_ADDR, "Invalid IPV4 address");
            return -1;
        }

        if (netmask_str == NULL) {
            SCRadixAddKeyIPV4((uint8_t *)ipv4_addr, sc_hinfo_tree,
                              (void *)user_data);
        } else {
            netmask_value = atoi(netmask_str);
            if (netmask_value < 0 || netmask_value > 32) {
                SCLogError(SC_ERR_INVALID_IP_NETBLOCK, "Invalid IPV4 Netblock");
                SCFree(ipv4_addr);
                return -1;
            }

            SCHInfoMaskIPNetblock((uint8_t *)ipv4_addr, netmask_value, 32);
            SCRadixAddKeyIPV4Netblock((uint8_t *)ipv4_addr, sc_hinfo_tree,
                                      (void *)user_data, netmask_value);
        }
    } else {
        /* if we are here, we have an IPV6 address */
        if ( (ipv6_addr = SCHInfoValidateIPV6Address(ip_str)) == NULL) {
            SCLogError(SC_ERR_INVALID_IPV6_ADDR, "Invalid IPV6 address inside");
            return -1;
        }

        if (netmask_str == NULL) {
            SCRadixAddKeyIPV6((uint8_t *)ipv6_addr, sc_hinfo_tree,
                              (void *)user_data);
        } else {
            netmask_value = atoi(netmask_str);
            if (netmask_value < 0 || netmask_value > 128) {
                SCLogError(SC_ERR_INVALID_IP_NETBLOCK, "Invalid IPV6 Netblock");
                SCFree(ipv6_addr);
                return -1;
            }

            SCHInfoMaskIPNetblock((uint8_t *)ipv6_addr, netmask_value, 128);
            SCRadixAddKeyIPV6Netblock((uint8_t *)ipv6_addr, sc_hinfo_tree,
                                      (void *)user_data, netmask_value);
        }
    }

    if (recursive == TRUE) {
        SCHInfoAddHostOSInfo(host_os, ip_str_rem, is_ipv4);
    }

    if (ip_str != NULL)
        SCFree(ip_str);
    if (ipv4_addr != NULL)
        SCFree(ipv4_addr);
    if (ipv6_addr != NULL)
        SCFree(ipv6_addr);
    return *user_data;
}

/**
 * \brief Retrieves the host os flavour, given an ipv4/ipv6 address as a string.
 *
 * \param Pointer to a string containing an IP address
 *
 * \retval The OS flavour on success; -1 on failure, or on not finding the key
 */
int SCHInfoGetHostOSFlavour(char *ip_addr_str)
{
    SCRadixNode *node = NULL;
    struct in_addr *ipv4_addr = NULL;
    struct in6_addr *ipv6_addr = NULL;

    if (ip_addr_str == NULL || index(ip_addr_str, '/') != NULL)
        return -1;

    if (index(ip_addr_str, ':') != NULL) {
        if ( (ipv6_addr = SCHInfoValidateIPV6Address(ip_addr_str)) == NULL) {
            SCLogError(SC_ERR_INVALID_IPV4_ADDR, "Invalid IPV4 address");
            return -1;
        }

        if ( (node = SCRadixFindKeyIPV6BestMatch((uint8_t *)ipv6_addr, sc_hinfo_tree)) == NULL)
            return -1;
        else
            return *((int *)node->prefix->user_data_result);
    } else {
        if ( (ipv4_addr = SCHInfoValidateIPV4Address(ip_addr_str)) == NULL) {
            SCLogError(SC_ERR_INVALID_IPV4_ADDR, "Invalid IPV4 address");
            return -1;
        }

        if ( (node = SCRadixFindKeyIPV4BestMatch((uint8_t *)ipv4_addr, sc_hinfo_tree)) == NULL)
            return -1;
        else
            return *((int *)node->prefix->user_data_result);
    }
}

/**
 * \brief Retrieves the host os flavour, given an ipv4 address in the raw
 *        address format.
 *
 * \param Pointer to a raw ipv4 address.
 *
 * \retval The OS flavour on success; -1 on failure, or on not finding the key
 */
int SCHInfoGetIPv4HostOSFlavour(uint8_t *ipv4_addr)
{
    SCRadixNode *node = SCRadixFindKeyIPV4BestMatch(ipv4_addr, sc_hinfo_tree);
    if (node == NULL)
        return -1;
    else
        return *((int *)node->prefix->user_data_result);
}

/**
 * \brief Retrieves the host os flavour, given an ipv6 address in the raw
 *        address format.
 *
 * \param Pointer to a raw ipv6 address.
 *
 * \retval The OS flavour on success; -1 on failure, or on not finding the key
 */
int SCHInfoGetIPv6HostOSFlavour(uint8_t *ipv6_addr)
{
    SCRadixNode *node = SCRadixFindKeyIPV6BestMatch(ipv6_addr, sc_hinfo_tree);
    if (node == NULL)
        return -1;
    else
        return *((int *)node->prefix->user_data_result);
}

void SCHInfoCleanResources(void)
{
    if (sc_hinfo_tree != NULL) {
        SCRadixReleaseRadixTree(sc_hinfo_tree);
        sc_hinfo_tree = NULL;
    }

    return;
}

/**
 * \brief Load the host os policy information from the configuration.
 *
 * \initonly (A mem alloc error should cause an exit failure)
 */
void SCHInfoLoadFromConfig(void)
{
    ConfNode *root = ConfGetNode("host-os-policy");
    if (root == NULL)
        return;

    ConfNode *policy;
    TAILQ_FOREACH(policy, &root->head, next) {
        ConfNode *host;
        TAILQ_FOREACH(host, &policy->head, next) {
            int is_ipv4 = 1;
            if (index(host->val, ':') != NULL)
                is_ipv4 = 0;
            if (SCHInfoAddHostOSInfo(policy->name, host->val, is_ipv4) == -1) {
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                    "Failed to add host \"%s\" with policy \"%s\" to host "
                    "info database", host->val, policy->name);
                exit(EXIT_FAILURE);
            }
        }
    }
}

/*------------------------------------Unit_Tests------------------------------*/

#ifdef UNITTESTS
static SCRadixTree *sc_hinfo_tree_backup = NULL;

static void SCHInfoCreateContextBackup(void)
{
    sc_hinfo_tree_backup = sc_hinfo_tree;
    sc_hinfo_tree = NULL;

    return;
}

static void SCHInfoRestoreContextBackup(void)
{
    sc_hinfo_tree = sc_hinfo_tree_backup;
    sc_hinfo_tree_backup = NULL;

    return;
}

/**
 * \test Check if we the IPs with the right OS flavours are added to the host OS
 *       radix tree, and the IPS with invalid flavours returns an error(-1)
 */
int SCHInfoTestInvalidOSFlavour01(void)
{
    SCHInfoCreateContextBackup();

    int result = 0;

    if (SCHInfoAddHostOSInfo("bamboo", "192.168.1.1", SC_HINFO_IS_IPV4) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("windows", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux10", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("hpux10", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux11", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("irix", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("irix", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("bsd", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("bsd", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("old_linux", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("old_linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("macos", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("macos", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows2k3", "192.168.1.1", SC_HINFO_IS_IPV4) !=
        SCMapEnumNameToValue("windows2k3", sc_hinfo_os_policy_map)) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check that invalid ipv4 addresses and ipv4 netblocks are rejected by
 *       the host os info API
 */
int SCHInfoTestInvalidIPV4Address02(void)
{
    SCHInfoCreateContextBackup();

    int result = 1;

    if (SCHInfoAddHostOSInfo("linux", "192.168.1.566", SC_HINFO_IS_IPV4) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "192.168.1", SC_HINFO_IS_IPV4 != -1)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "192.", SC_HINFO_IS_IPV4) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "192.168", SC_HINFO_IS_IPV4) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "", SC_HINFO_IS_IPV4) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "192.168.1.1/33", SC_HINFO_IS_IPV4) != -1) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check that invalid ipv4 addresses and ipv4 netblocks are rejected by
 *       the host os info API
 */
int SCHInfoTestInvalidIPV6Address03(void)
{
    SCHInfoCreateContextBackup();

    int result = 1;

    if (SCHInfoAddHostOSInfo("linux", "2362:7322", SC_HINFO_IS_IPV6) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "19YW:", SC_HINFO_IS_IPV6) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "1235", SC_HINFO_IS_IPV6) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "1922:236115:", SC_HINFO_IS_IPV6) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "", SC_HINFO_IS_IPV6) != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux",
                             "1921.6311:6241:6422:7352:ABBB:DDDD:EEEE/129",
                             SC_HINFO_IS_IPV6) != -1) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check that valid ipv4 addresses are inserted into the host os radix
 *       tree, and the host os api retrieves the right value for the host os
 *       flavour, on supplying as arg an ipv4 addresses that has been added to
 *       the host os radix tree.
 */
int SCHInfoTestValidIPV4Address04(void)
{
    SCHInfoCreateContextBackup();

    int result = 1;

    if (SCHInfoAddHostOSInfo("linux", "192.168.1.1", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows", "192.192.1.2", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "192.168.1.100", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux10", "192.168.2.4", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "192.192.1.5", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista", "192.168.10.20", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "111.163.151.62", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "11.1.120.210", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "19.18.110.210", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows", "19.18.120.110", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux11", "191.168.11.128", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista", "191.168.11.192", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }

    if (SCHInfoGetHostOSFlavour("192.168.1.1") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.1.2") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.1.100") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.192.2.4") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.2.4") !=
        SCMapEnumNameToValue("hpux10", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.192.1.5") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.10.20") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.163.151.62") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("11.1.120.210") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("19.18.110.210") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("19.18.120.110") !=
        SCMapEnumNameToValue("windows", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("191.168.11.128") !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("191.168.11.192") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("191.168.11.224") != -1) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check that valid ipv4 addresses/netblocks are inserted into the host os
 *       radix tree, and the host os api retrieves the right value for the host
 *       os flavour, on supplying as arg an ipv4 addresses that has been added
 *       to the host os radix tree.
 */
int SCHInfoTestValidIPV4Address05(void)
{
    SCHInfoCreateContextBackup();

    struct in_addr in;
    int result = 1;

    if (SCHInfoAddHostOSInfo("linux", "192.168.1.1", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows", "192.192.1.2", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "192.168.1.100", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux10", "192.168.2.4", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "192.192.1.5", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista", "192.168.10.20", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "111.163.151.62", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux11", "111.162.208.124/20", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows", "111.162.240.1", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "111.162.214.100", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista", "111.162.208.100", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux", "111.162.194.112", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }

    if (SCHInfoGetHostOSFlavour("192.168.1.1") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.1.2") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.1.100") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.192.2.4") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.2.4") !=
        SCMapEnumNameToValue("hpux10", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.192.1.5") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.10.20") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.163.151.62") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.208.0") !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.210.1") !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.214.1") !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.0.0") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.240.112") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.240.1") !=
        SCMapEnumNameToValue("windows", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.214.100") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (inet_pton(AF_INET, "111.162.208.100", &in) < 0) {
        goto end;
    }
    if (SCHInfoGetIPv4HostOSFlavour((uint8_t *)&in) !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.194.112") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.208.200") !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (inet_pton(AF_INET, "111.162.208.200", &in) < 0) {
        goto end;
    }
    if (SCHInfoGetIPv4HostOSFlavour((uint8_t *)&in) !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("111.162.200.201") != -1) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check that valid ipv6 addresses are inserted into the host os radix
 *       tree, and the host os api retrieves the right value for the host os
 *       flavour, on supplying as arg an ipv6 address that has been added to
 *       the host os radix tree.
 */
int SCHInfoTestValidIPV6Address06(void)
{
    SCHInfoCreateContextBackup();

    int result = 1;

    if (SCHInfoAddHostOSInfo("linux",
                             "2351:2512:6211:6246:235A:6242:2352:62AD",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows",
                             "6961:6121:2132:6241:423A:2135:2461:621D",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "DD13:613D:F312:62DD:6213:421A:6212:2652",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux10",
                             "9891:2131:2151:6426:1342:674D:622F:2342",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux",
                             "3525:2351:4223:6211:2311:2667:6242:2154",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista",
                             "1511:6211:6726:7777:1212:2333:6222:7722",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "2666:6222:7222:2335:6223:7722:3425:2362",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "8762:2352:6241:7245:EE23:21AD:2312:622C",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux",
                             "6422:EE1A:2621:34AD:2462:432D:642E:E13A",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows",
                             "3521:7622:6241:6242:7277:1234:2352:6234",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux11",
                             "2141:6232:6252:2223:7734:2345:6245:6222",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista",
                             "5222:6432:6432:2322:6662:3423:4322:3245",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }

    if (SCHInfoGetHostOSFlavour("2351:2512:6211:6246:235A:6242:2352:62AD") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("2351:2512:6211:6246:235A:6242:2352:6FFFE") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("DD13:613D:F312:62DD:6213:421A:6212:2652") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("DD13:613D:F312:62DD:6213:421A:6212:2222") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("9891:2131:2151:6426:1342:674D:622F:2342") !=
        SCMapEnumNameToValue("hpux10", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("3525:2351:4223:6211:2311:2667:6242:2154") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("1511:6211:6726:7777:1212:2333:6222:7722") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("2666:6222:7222:2335:6223:7722:3425:2362") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2312:622C") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("6422:EE1A:2621:34AD:2462:432D:642E:E13A") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("3521:7622:6241:6242:7277:1234:2352:6234") !=
        SCMapEnumNameToValue("windows", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("2141:6232:6252:2223:7734:2345:6245:6222") !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("5222:6432:6432:2322:6662:3423:4322:3245") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("5222:6432:6432:2322:6662:3423:4322:DDDD") != -1) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check that valid ipv6 addresses/netblocks are inserted into the host os
 *       radix tree, and the host os api retrieves the right value for the host
 *       os flavour, on supplying as arg an ipv6 address that has been added to
 *       the host os radix tree.
 */
int SCHInfoTestValidIPV6Address07(void)
{
    SCHInfoCreateContextBackup();

    int result = 1;

    if (SCHInfoAddHostOSInfo("linux",
                             "2351:2512:6211:6246:235A:6242:2352:62AD",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows",
                             "6961:6121:2132:6241:423A:2135:2461:621D",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "DD13:613D:F312:62DD:6213:421A:6212:2652",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux10",
                             "9891:2131:2151:6426:1342:674D:622F:2342",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux",
                             "3525:2351:4223:6211:2311:2667:6242:2154",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista",
                             "1511:6211:6726:7777:1212:2333:6222:7722",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "2666:6222:7222:2335:6223:7722:3425:2362",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "8762:2352:6241:7245:EE23:21AD:2312:622C/68",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux",
                             "8762:2352:6241:7245:EE23:21AD:2412:622C",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows",
                             "8762:2352:6241:7245:EE23:21AD:FFFF:622C",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux11",
                             "8762:2352:6241:7245:EE23:21AD:2312:62FF",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista",
                             "8762:2352:6241:7245:EE23:21AD:2121:1212",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }

    if (SCHInfoGetHostOSFlavour("2351:2512:6211:6246:235A:6242:2352:62AD") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("2351:2512:6211:6246:235A:6242:2352:6FFFE") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("DD13:613D:F312:62DD:6213:421A:6212:2652") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("DD13:613D:F312:62DD:6213:421A:6212:2222") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("9891:2131:2151:6426:1342:674D:622F:2342") !=
        SCMapEnumNameToValue("hpux10", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("3525:2351:4223:6211:2311:2667:6242:2154") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("1511:6211:6726:7777:1212:2333:6222:7722") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("2666:6222:7222:2335:6223:7722:3425:2362") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2312:622C") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2412:622C") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:FFFF:622C") !=
        SCMapEnumNameToValue("windows", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2312:62FF") !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2121:1212") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("5222:6432:6432:2322:6662:3423:4322:DDDD") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2121:1DDD") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:FFFF:2121:1DDD") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2312:622C") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE00:0000:0000:0000") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:E000:0000:0000:0000") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check that valid ipv6 addresses/netblocks are inserted into the host os
 *       radix tree, and the host os api retrieves the right value for the host
 *       os flavour, on supplying as arg an ipv6 address that has been added to
 *       the host os radix tree.
 */
int SCHInfoTestValidIPV6Address08(void)
{
    SCHInfoCreateContextBackup();

    struct in6_addr in6;
    int result = 1;

    if (SCHInfoAddHostOSInfo("linux",
                             "2351:2512:6211:6246:235A:6242:2352:62AD",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows",
                             "6961:6121:2132:6241:423A:2135:2461:621D",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "DD13:613D:F312:62DD:6213:421A:6212:2652",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux10",
                             "9891:2131:2151:6426:1342:674D:622F:2342",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux",
                             "3525:2351:4223:6211:2311:2667:6242:2154",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista",
                             "1511:6211:6726:7777:1212:2333:6222:7722",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "2666:6222:7222:2335:6223:7722:3425:2362",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris",
                             "8762:2352:6241:7245:EE23:21AD:2312:622C/68",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("linux",
                             "8762:2352:6241:7245:EE23:21AD:2412:622C",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("windows",
                             "8762:2352:6241:7245:EE23:21AD:FFFF:622C",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("hpux11",
                             "8762:2352:6241:7245:EE23:21AD:2312:62FF",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista",
                             "8762:2352:6241:7245:EE23:21AD:2121:1212",
                             SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("irix", "default", SC_HINFO_IS_IPV6) == -1) {
        goto end;
    }

    if (SCHInfoGetHostOSFlavour("2351:2512:6211:6246:235A:6242:2352:62AD") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("2351:2512:6211:6246:235A:6242:2352:6FFF") !=
        SCMapEnumNameToValue("irix", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("DD13:613D:F312:62DD:6213:421A:6212:2652") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("DD13:613D:F312:62DD:6213:421A:6212:2222") !=
        SCMapEnumNameToValue("irix", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("9891:2131:2151:6426:1342:674D:622F:2342") !=
        SCMapEnumNameToValue("hpux10", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("3525:2351:4223:6211:2311:2667:6242:2154") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("1511:6211:6726:7777:1212:2333:6222:7722") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("2666:6222:7222:2335:6223:7722:3425:2362") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2312:622C") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2412:622C") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:FFFF:622C") !=
        SCMapEnumNameToValue("windows", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2312:62FF") !=
        SCMapEnumNameToValue("hpux11", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2121:1212") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("5222:6432:6432:2322:6662:3423:4322:DDDD") !=
        SCMapEnumNameToValue("irix", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2121:1DDD") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:FFFF:2121:1DDD") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE23:21AD:2312:622C") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("8762:2352:6241:7245:EE00:0000:0000:0000") !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (inet_pton(AF_INET6, "8762:2352:6241:7245:E000:0000:0000:0000", &in6) < 0) {
        goto end;
    }
    if (SCHInfoGetIPv6HostOSFlavour((uint8_t *)&in6) !=
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (inet_pton(AF_INET6, "AD23:2DDA:6D1D:A223:E235:0232:1241:1666", &in6) < 0) {
        goto end;
    }
    if (SCHInfoGetIPv6HostOSFlavour((uint8_t *)&in6) !=
        SCMapEnumNameToValue("irix", sc_hinfo_os_policy_map)) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check that valid ipv4 addresses are inserted into the host os radix
 *       tree, and the host os api retrieves the right value for the host os
 *       flavour, on supplying as arg an ipv4 addresses that has been added to
 *       the host os radix tree.
 */
int SCHInfoTestValidIPV4Address09(void)
{
    SCHInfoCreateContextBackup();

    int result = 1;

    if (SCHInfoAddHostOSInfo("linux", "192.168.1.0", SC_HINFO_IS_IPV4) == -1) {
                goto end;
    }
    if (SCHInfoAddHostOSInfo("windows", "192.192.1.2", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.1.0") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "192.168.1.0/16", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("macos", "192.168.1.0/20", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.1.0") !=
        SCMapEnumNameToValue("linux", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("vista", "192.168.50.128/25", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.50.128") !=
        SCMapEnumNameToValue("vista", sc_hinfo_os_policy_map)) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("irix", "192.168.50.128", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("192.168.50.128") !=
        SCMapEnumNameToValue("irix", sc_hinfo_os_policy_map)) {
        goto end;
    }

    if (SCHInfoGetHostOSFlavour("192.168.1.100") !=
        SCMapEnumNameToValue("macos", sc_hinfo_os_policy_map)) {
        goto end;
    }

    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0) {
        goto end;
    }

    SCRadixRemoveKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, sc_hinfo_tree, 16);

    if (SCHInfoGetHostOSFlavour("192.168.1.100") !=
        SCMapEnumNameToValue("macos", sc_hinfo_os_policy_map)) {
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0) {
        goto end;
    }
    SCRadixRemoveKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, sc_hinfo_tree, 20);

    if (SCHInfoGetHostOSFlavour("192.168.1.100") != -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("solaris", "192.168.1.0/16", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }
    if (SCHInfoAddHostOSInfo("macos", "192.168.1.0/20", SC_HINFO_IS_IPV4) == -1) {
        goto end;
    }

    if (SCHInfoGetHostOSFlavour("192.168.1.100") ==
        SCMapEnumNameToValue("macos", sc_hinfo_os_policy_map)) {
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0) {
        goto end;
    }
    SCRadixRemoveKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, sc_hinfo_tree, 20);

    if (SCHInfoGetHostOSFlavour("192.168.1.100") ==
        SCMapEnumNameToValue("solaris", sc_hinfo_os_policy_map)) {
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0) {
        goto end;
    }
    SCRadixRemoveKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, sc_hinfo_tree, 16);

    if (SCHInfoGetHostOSFlavour("192.168.1.100") != -1) {
        goto end;
    }

    result = 1;

 end:
    SCHInfoCleanResources();
    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check the loading of host info from a configuration file.
 */
int SCHInfoTestLoadFromConfig01(void)
{
    char config[] = "\
%YAML 1.1\n\
---\n\
host-os-policy:\n\
  bsd: [0.0.0.0/0]\n\
  windows: [10.0.0.0/8, 192.168.1.0/24]\n\
  linux: [10.0.0.5/32]\n\
\n";

    int result = 0;

    SCHInfoCreateContextBackup();

    ConfCreateContextBackup();
    ConfInit();
    ConfYamlLoadString(config, strlen(config));

    SCHInfoLoadFromConfig();
    if (SCHInfoGetHostOSFlavour("10.0.0.4") != OS_POLICY_WINDOWS)
        goto end;
    if (SCHInfoGetHostOSFlavour("10.0.0.5") != OS_POLICY_LINUX)
        goto end;
    if (SCHInfoGetHostOSFlavour("192.168.1.1") != OS_POLICY_WINDOWS)
        goto end;
    if (SCHInfoGetHostOSFlavour("172.168.1.1") != OS_POLICY_BSD)
        goto end;

    result = 1;

 end:
    ConfDeInit();
    ConfRestoreContextBackup();

    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check the loading of host info from a configuration file.
 */
int SCHInfoTestLoadFromConfig02(void)
{
    char config[] = "\
%YAML 1.1\n\
---\n\
host-os-policy:\n\
  one-two: [0.0.0.0/0]\n\
  one-two-three:\n\
  four_five:\n\
  six-seven_eight: [10.0.0.0/8, 192.168.1.0/24]\n\
  nine_ten_eleven: [10.0.0.5/32]\n\
\n";

    int result = 0;

    SCHInfoCreateContextBackup();

    ConfCreateContextBackup();
    ConfInit();
    ConfYamlLoadString(config, strlen(config));

    ConfNode *root = ConfGetNode("host-os-policy");
    if (root == NULL)
        goto end;

    int count = 0;

    ConfNode *policy;
    TAILQ_FOREACH(policy, &root->head, next) {
        switch (count) {
            case 0:
                if (strcmp("one-two", policy->name) != 0)
                    goto end;
                break;
            case 1:
                if (strcmp("one-two-three", policy->name) != 0)
                    goto end;
                break;
            case 2:
                if (strcmp("four-five", policy->name) != 0)
                    goto end;
                break;
            case 3:
                if (strcmp("six-seven-eight", policy->name) != 0)
                    goto end;
                break;
            case 4:
                if (strcmp("nine-ten-eleven", policy->name) != 0)
                    goto end;
                break;
        }
        count++;
    }

    result = 1;

 end:
    ConfDeInit();
    ConfRestoreContextBackup();

    SCHInfoRestoreContextBackup();

    return result;
}

/**
 * \test Check the loading of host info from a configuration file.
 */
int SCHInfoTestLoadFromConfig03(void)
{
    char config[] = "\
%YAML 1.1\n\
---\n\
host-os-policy:\n\
  bsd-right: [0.0.0.1]\n\
  old-linux: [0.0.0.2]\n\
  old-solaris: [0.0.0.3]\n\
  windows: [0.0.0.4]\n\
  vista: [0.0.0.5]\n\
\n";

    int result = 1;

    SCHInfoCreateContextBackup();

    ConfCreateContextBackup();
    ConfInit();
    ConfYamlLoadString(config, strlen(config));

    ConfNode *root = ConfGetNode("host-os-policy");
    if (root == NULL)
        goto end;

    ConfNode *policy;
    TAILQ_FOREACH(policy, &root->head, next) {
        if (SCMapEnumNameToValue(policy->name, sc_hinfo_os_policy_map) == -1) {
            printf("Invalid enum map inside\n");
            goto end;
        }
    }

    result = 1;

 end:
    ConfDeInit();
    ConfRestoreContextBackup();

    SCHInfoRestoreContextBackup();
    return result;
}

/**
 * \test Check the loading of host info from a configuration file.
 */
int SCHInfoTestLoadFromConfig04(void)
{
    char config[] = "\
%YAML 1.1\n\
---\n\
host-os-policy:\n\
  bsd_right: [0.0.0.1]\n\
  old_linux: [0.0.0.2]\n\
  old_solaris: [0.0.0.3]\n\
  windows: [0.0.0.4]\n\
  vista: [0.0.0.5]\n\
\n";

    int result = 1;

    SCHInfoCreateContextBackup();

    ConfCreateContextBackup();
    ConfInit();
    ConfYamlLoadString(config, strlen(config));

    ConfNode *root = ConfGetNode("host-os-policy");
    if (root == NULL)
        goto end;

    ConfNode *policy;
    TAILQ_FOREACH(policy, &root->head, next) {
        if (SCMapEnumNameToValue(policy->name, sc_hinfo_os_policy_map) == -1) {
            printf("Invalid enum map inside\n");
            goto end;
        }
    }

    result = 1;

 end:
    ConfDeInit();
    ConfRestoreContextBackup();

    SCHInfoRestoreContextBackup();
    return result;
}

/**
 * \test Check the loading of host info from a configuration file.
 */
int SCHInfoTestLoadFromConfig05(void)
{
    char config[] = "\
%YAML 1.1\n\
---\n\
host-os-policy:\n\
  bsd_right: [0.0.0.1]\n\
  old_linux: [0.0.0.2]\n\
  old-solaris: [0.0.0.3]\n\
  windows: [0.0.0.4]\n\
  linux: [0.0.0.5]\n\
\n";

    int result = 0;

    SCHInfoCreateContextBackup();

    ConfCreateContextBackup();
    ConfInit();
    ConfYamlLoadString(config, strlen(config));

    SCHInfoLoadFromConfig();

    if (SCHInfoGetHostOSFlavour("0.0.0.1") != OS_POLICY_BSD_RIGHT) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("0.0.0.2") != OS_POLICY_OLD_LINUX) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("0.0.0.3") != OS_POLICY_OLD_SOLARIS) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("0.0.0.4") != OS_POLICY_WINDOWS) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("0.0.0.5") != OS_POLICY_VISTA) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("0.0.0.0") != -1) {
        goto end;
    }
    if (SCHInfoGetHostOSFlavour("0.0.0.6") != -1) {
        goto end;
    }


    result = 1;

 end:
    ConfDeInit();
    ConfRestoreContextBackup();

    SCHInfoRestoreContextBackup();

    return result;
}

#endif /* UNITTESTS */

void SCHInfoRegisterTests(void)
{

#ifdef UNITTESTS

    UtRegisterTest("SCHInfoTesInvalidOSFlavour01",
                   SCHInfoTestInvalidOSFlavour01, 1);
    UtRegisterTest("SCHInfoTestInvalidIPV4Address02",
                   SCHInfoTestInvalidIPV4Address02, 1);
    UtRegisterTest("SCHInfoTestInvalidIPV6Address03",
                   SCHInfoTestInvalidIPV6Address03, 1);
    UtRegisterTest("SCHInfoTestValidIPV4Address04",
                   SCHInfoTestValidIPV4Address04, 1);
    UtRegisterTest("SCHInfoTestValidIPV4Address05",
                   SCHInfoTestValidIPV4Address05, 1);
    UtRegisterTest("SCHInfoTestValidIPV6Address06",
                   SCHInfoTestValidIPV6Address06, 1);
    UtRegisterTest("SCHInfoTestValidIPV6Address07",
                   SCHInfoTestValidIPV6Address07, 1);
    UtRegisterTest("SCHInfoTestValidIPV6Address08",
                   SCHInfoTestValidIPV6Address08, 1);
    UtRegisterTest("SCHInfoTestValidIPV4Address09",
                   SCHInfoTestValidIPV4Address09, 1);

    UtRegisterTest("SCHInfoTestLoadFromConfig01",
                   SCHInfoTestLoadFromConfig01, 1);
    UtRegisterTest("SCHInfoTestLoadFromConfig02",
                   SCHInfoTestLoadFromConfig02, 1);
    UtRegisterTest("SCHInfoTestLoadFromConfig03",
                   SCHInfoTestLoadFromConfig03, 1);
    UtRegisterTest("SCHInfoTestLoadFromConfig04",
                   SCHInfoTestLoadFromConfig04, 1);
#endif /* UNITTESTS */

}
