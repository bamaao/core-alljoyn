/******************************************************************************
 * Copyright AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/
#include <gtest/gtest.h>
#include <alljoyn/AuthListener.h>
#include <alljoyn/BusAttachment.h>
#include <alljoyn/SecurityApplicationProxy.h>
#include <qcc/Util.h>
#include <map>

#include "InMemoryKeyStore.h"
#include "PermissionMgmtObj.h"
#include "PermissionMgmtTest.h"
#include "KeyStore.h"
#include "ajTestCommon.h"

#define TEN_MINS 600 // 600 secs is 10 mins

using namespace ajn;
using namespace qcc;
using namespace std;

class SecurityManagement_ApplicationStateListener : public ApplicationStateListener {
  public:
    SecurityManagement_ApplicationStateListener() : stateMap() { }

    virtual void State(const char* busName, const qcc::KeyInfoNISTP256& publicKeyInfo, PermissionConfigurator::ApplicationState state) {
        QCC_UNUSED(publicKeyInfo);
        stateMap[busName] = state;
    }

    bool isClaimed(const String& busName) {
        if (stateMap.count(busName) > 0) {
            if (stateMap.find(busName)->second == PermissionConfigurator::ApplicationState::CLAIMED) {
                return true;
            }
        }
        return false;
    }
    map<String, PermissionConfigurator::ApplicationState> stateMap;
};

class SecurityManagementTestSessionPortListener : public SessionPortListener {
  public:
    virtual bool AcceptSessionJoiner(SessionPort sessionPort, const char* joiner, const SessionOpts& opts) {
        QCC_UNUSED(sessionPort);
        QCC_UNUSED(joiner);
        QCC_UNUSED(opts);
        return true;
    }
};

class SecurityManagementTestBusObject : public BusObject {
  public:
    SecurityManagementTestBusObject(BusAttachment& bus, const char* path, const char* interfaceName, bool announce = true)
        : BusObject(path), isAnnounced(announce), prop1(42), prop2(17) {
        const InterfaceDescription* iface = bus.GetInterface(interfaceName);
        EXPECT_TRUE(iface != NULL) << "NULL InterfaceDescription* for " << interfaceName;
        if (iface == NULL) {
            printf("The interfaceDescription pointer for %s was NULL when it should not have been.\n", interfaceName);
            return;
        }

        if (isAnnounced) {
            AddInterface(*iface, ANNOUNCED);
        } else {
            AddInterface(*iface, UNANNOUNCED);
        }

        /* Register the method handlers with the object */
        const MethodEntry methodEntries[] = {
            { iface->GetMember("Echo"), static_cast<MessageReceiver::MethodHandler>(&SecurityManagementTestBusObject::Echo) }
        };
        EXPECT_EQ(ER_OK, AddMethodHandlers(methodEntries, sizeof(methodEntries) / sizeof(methodEntries[0])));
    }

    void Echo(const InterfaceDescription::Member* member, Message& msg) {
        QCC_UNUSED(member);
        const MsgArg* arg((msg->GetArg(0)));
        QStatus status = MethodReply(msg, arg, 1);
        EXPECT_EQ(ER_OK, status) << "Echo: Error sending reply";
    }

    QStatus Get(const char* ifcName, const char* propName, MsgArg& val)
    {
        QCC_UNUSED(ifcName);
        QStatus status = ER_OK;
        if (0 == strcmp("Prop1", propName)) {
            val.Set("i", prop1);
        } else if (0 == strcmp("Prop2", propName)) {
            val.Set("i", prop2);
        } else {
            status = ER_BUS_NO_SUCH_PROPERTY;
        }
        return status;

    }

    QStatus Set(const char* ifcName, const char* propName, MsgArg& val)
    {
        QCC_UNUSED(ifcName);
        QStatus status = ER_OK;
        if ((0 == strcmp("Prop1", propName)) && (val.typeId == ALLJOYN_INT32)) {
            val.Get("i", &prop1);
        } else if ((0 == strcmp("Prop2", propName)) && (val.typeId == ALLJOYN_INT32)) {
            val.Get("i", &prop2);
        } else {
            status = ER_BUS_NO_SUCH_PROPERTY;
        }
        return status;
    }
    int32_t ReadProp1() {
        return prop1;
    }
  private:
    bool isAnnounced;
    int32_t prop1;
    int32_t prop2;
};

class ChirpSignalReceiver : public MessageReceiver {
  public:
    ChirpSignalReceiver() : signalReceivedFlag(false) { }
    void ChirpSignalHandler(const InterfaceDescription::Member* member,
                            const char* sourcePath, Message& msg) {
        QCC_UNUSED(member);
        QCC_UNUSED(sourcePath);
        QCC_UNUSED(msg);
        signalReceivedFlag = true;
    }
    bool signalReceivedFlag;
};

static void GetAppPublicKey(BusAttachment& bus, ECCPublicKey& publicKey)
{
    KeyInfoNISTP256 keyInfo;
    bus.GetPermissionConfigurator().GetSigningPublicKey(keyInfo);
    publicKey = *keyInfo.GetPublicKey();
}

class SecurityManagementTestConfigurationListener : public PermissionConfigurationListener {
  public:
    SecurityManagementTestConfigurationListener() : factoryResetReceived(false), policyChangedReceived(false),
        startManagementReceived(false), endManagementReceived(false) {
    }

    QStatus FactoryReset();
    bool factoryResetReceived;

    void PolicyChanged();
    bool policyChangedReceived;

    void StartManagement();
    bool startManagementReceived;

    void EndManagement();
    bool endManagementReceived;
};

QStatus SecurityManagementTestConfigurationListener::FactoryReset()
{
    factoryResetReceived = true;
    return ER_OK;
}

void SecurityManagementTestConfigurationListener::PolicyChanged()
{
    policyChangedReceived = true;
}

void SecurityManagementTestConfigurationListener::StartManagement()
{
    startManagementReceived = true;
}

void SecurityManagementTestConfigurationListener::EndManagement()
{
    endManagementReceived = true;
}

class SecurityManagementPolicyTest : public testing::Test {
  public:
    SecurityManagementPolicyTest() :
        managerBus("SecurityPolicyRulesManager"),
        peer1Bus("SecurityPolicyRulesPeer1"),
        peer2Bus("SecurityPolicyRulesPeer2"),
        peer3Bus("SecurityPolicyRulesPeer3"),
        managerSessionPort(42),
        peer1SessionPort(42),
        peer2SessionPort(42),
        managerToManagerSessionId(0),
        managerToPeer1SessionId(0),
        managerToPeer2SessionId(0),
        interfaceName("org.allseen.test.SecurityApplication.rules"),
        managerAuthListener(NULL),
        peer1AuthListener(NULL),
        peer2AuthListener(NULL),
        peer3AuthListener(NULL),
        appStateListener()
    {
    }

    virtual void SetUp() {
        EXPECT_EQ(ER_OK, managerBus.Start());
        EXPECT_EQ(ER_OK, managerBus.Connect());
        EXPECT_EQ(ER_OK, peer1Bus.Start());
        EXPECT_EQ(ER_OK, peer1Bus.Connect());
        EXPECT_EQ(ER_OK, peer2Bus.Start());
        EXPECT_EQ(ER_OK, peer2Bus.Connect());
        EXPECT_EQ(ER_OK, peer3Bus.Start());
        EXPECT_EQ(ER_OK, peer3Bus.Connect());

        // Register in memory keystore listeners
        EXPECT_EQ(ER_OK, managerBus.RegisterKeyStoreListener(managerKeyStoreListener));
        EXPECT_EQ(ER_OK, peer1Bus.RegisterKeyStoreListener(peer1KeyStoreListener));
        EXPECT_EQ(ER_OK, peer2Bus.RegisterKeyStoreListener(peer2KeyStoreListener));
        EXPECT_EQ(ER_OK, peer3Bus.RegisterKeyStoreListener(peer3KeyStoreListener));

        managerAuthListener = new DefaultECDHEAuthListener();
        peer1AuthListener = new DefaultECDHEAuthListener();
        peer2AuthListener = new DefaultECDHEAuthListener();
        peer3AuthListener = new DefaultECDHEAuthListener();

        EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", managerAuthListener, nullptr, false, &managerConfigurationListener));
        EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", peer1AuthListener, nullptr, false, &peer1ConfigurationListener));
        EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", peer2AuthListener, nullptr, false, &peer2ConfigurationListener));
        EXPECT_EQ(ER_OK, peer3Bus.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", peer3AuthListener, nullptr, false, &peer3ConfigurationListener));

        PermissionMgmtTestHelper::GetGUID(managerBus, managerGuid);
        SetManifestTemplate(managerBus);
        SetManifestTemplate(peer1Bus);
        SetManifestTemplate(peer2Bus);
        SetManifestTemplate(peer3Bus);

        // We are not marking the interface as a secure interface. Some of the
        // tests don't use security. So we use Object based security for any
        // test that security is required.
        interface = "<node>"
                    "<interface name='" + String(interfaceName) + "'>"
                    "  <method name='Echo'>"
                    "    <arg name='shout' type='s' direction='in'/>"
                    "    <arg name='reply' type='s' direction='out'/>"
                    "  </method>"
                    "  <signal name='Chirp'>"
                    "    <arg name='tweet' type='s'/>"
                    "  </signal>"
                    "  <property name='Prop1' type='i' access='readwrite'/>"
                    "  <property name='Prop2' type='i' access='readwrite'/>"
                    "</interface>"
                    "</node>";

        EXPECT_EQ(ER_OK, managerBus.CreateInterfacesFromXml(interface.c_str()));
        EXPECT_EQ(ER_OK, peer1Bus.CreateInterfacesFromXml(interface.c_str()));
        EXPECT_EQ(ER_OK, peer2Bus.CreateInterfacesFromXml(interface.c_str()));
        EXPECT_EQ(ER_OK, peer3Bus.CreateInterfacesFromXml(interface.c_str()));

        SessionOpts opts1;
        EXPECT_EQ(ER_OK, managerBus.BindSessionPort(managerSessionPort, opts1, managerSessionPortListener));

        SessionOpts opts2;
        EXPECT_EQ(ER_OK, peer1Bus.BindSessionPort(peer1SessionPort, opts2, peer1SessionPortListener));

        SessionOpts opts3;
        EXPECT_EQ(ER_OK, peer2Bus.BindSessionPort(peer2SessionPort, opts3, peer2SessionPortListener));

        EXPECT_EQ(ER_OK, managerBus.JoinSession(managerBus.GetUniqueName().c_str(), managerSessionPort, NULL, managerToManagerSessionId, opts1));
        EXPECT_EQ(ER_OK, managerBus.JoinSession(peer1Bus.GetUniqueName().c_str(), peer1SessionPort, NULL, managerToPeer1SessionId, opts2));
        EXPECT_EQ(ER_OK, managerBus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, managerToPeer2SessionId, opts3));

        SecurityApplicationProxy sapWithManager(managerBus, managerBus.GetUniqueName().c_str(), managerToManagerSessionId);
        PermissionConfigurator::ApplicationState applicationStateManager;
        EXPECT_EQ(ER_OK, sapWithManager.GetApplicationState(applicationStateManager));
        EXPECT_EQ(PermissionConfigurator::CLAIMABLE, applicationStateManager);

        SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
        PermissionConfigurator::ApplicationState applicationStatePeer1;
        EXPECT_EQ(ER_OK, sapWithPeer1.GetApplicationState(applicationStatePeer1));
        EXPECT_EQ(PermissionConfigurator::CLAIMABLE, applicationStatePeer1);

        SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);
        PermissionConfigurator::ApplicationState applicationStatePeer2;
        EXPECT_EQ(ER_OK, sapWithPeer2.GetApplicationState(applicationStatePeer2));
        EXPECT_EQ(PermissionConfigurator::CLAIMABLE, applicationStatePeer2);

        managerBus.RegisterApplicationStateListener(appStateListener);

        Manifest manifests[1];
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

        // Get manager key
        KeyInfoNISTP256 managerKey;
        PermissionConfigurator& pcManager = managerBus.GetPermissionConfigurator();
        EXPECT_EQ(ER_OK, pcManager.GetSigningPublicKey(managerKey));

        //Create peer1 key
        KeyInfoNISTP256 peer1Key;
        PermissionConfigurator& pcPeer1 = peer1Bus.GetPermissionConfigurator();
        EXPECT_EQ(ER_OK, pcPeer1.GetSigningPublicKey(peer1Key));

        // Create peer2 key
        KeyInfoNISTP256 peer2Key;
        PermissionConfigurator& pcPeer2 = peer2Bus.GetPermissionConfigurator();
        EXPECT_EQ(ER_OK, pcPeer2.GetSigningPublicKey(peer2Key));

        // Create identityCert
        const size_t certChainSize = 1;
        IdentityCertificate identityCertChainMaster[certChainSize];

        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateIdentityCert(managerBus,
                                                                      "0",
                                                                      managerGuid.ToString(),
                                                                      managerKey.GetPublicKey(),
                                                                      "ManagerAlias",
                                                                      3600,
                                                                      identityCertChainMaster[0])) << "Failed to create identity certificate.";

        SecurityApplicationProxy sapWithManagerBus(managerBus, managerBus.GetUniqueName().c_str());
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(managerBus, identityCertChainMaster[0], manifests[0]));
        EXPECT_EQ(ER_OK, sapWithManagerBus.Claim(managerKey,
                                                 managerGuid,
                                                 managerKey,
                                                 identityCertChainMaster, certChainSize,
                                                 manifests, ArraySize(manifests)));

        for (uint32_t msec = 0; msec < LOOP_END_10000; msec += WAIT_TIME_5) {
            if (appStateListener.isClaimed(managerBus.GetUniqueName())) {
                break;
            }
            qcc::Sleep(WAIT_TIME_5);
        }

        ECCPublicKey managerPublicKey;
        GetAppPublicKey(managerBus, managerPublicKey);
        ASSERT_EQ(*managerKey.GetPublicKey(), managerPublicKey);

        ASSERT_EQ(PermissionConfigurator::ApplicationState::CLAIMED, appStateListener.stateMap[managerBus.GetUniqueName()]);

        //Create peer1 identityCert
        IdentityCertificate identityCertChainPeer1[certChainSize];


        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateIdentityCert(managerBus,
                                                                      "0",
                                                                      managerGuid.ToString(),
                                                                      peer1Key.GetPublicKey(),
                                                                      "Peer1Alias",
                                                                      3600,
                                                                      identityCertChainPeer1[0])) << "Failed to create identity certificate.";

        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(managerBus, identityCertChainPeer1[0], manifests[0]));
        //Manager claims Peers
        EXPECT_EQ(ER_OK, sapWithPeer1.Claim(managerKey,
                                            managerGuid,
                                            managerKey,
                                            identityCertChainPeer1, certChainSize,
                                            manifests, ArraySize(manifests)));

        for (uint32_t msec = 0; msec < LOOP_END_10000; msec += WAIT_TIME_5) {
            if (appStateListener.isClaimed(peer1Bus.GetUniqueName())) {
                break;
            }
            qcc::Sleep(WAIT_TIME_5);
        }

        ASSERT_EQ(PermissionConfigurator::ApplicationState::CLAIMED, appStateListener.stateMap[peer1Bus.GetUniqueName()]);

        // Create peer2 identityCert
        IdentityCertificate identityCertChainPeer2[certChainSize];
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateIdentityCert(managerBus,
                                                                      "0",
                                                                      managerGuid.ToString(),
                                                                      peer2Key.GetPublicKey(),
                                                                      "Peer2Alias",
                                                                      3600,
                                                                      identityCertChainPeer2[0])) << "Failed to create identity certificate.";
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(managerBus, identityCertChainPeer2[0], manifests[0]));
        EXPECT_EQ(ER_OK, sapWithPeer2.Claim(managerKey,
                                            managerGuid,
                                            managerKey,
                                            identityCertChainPeer2, certChainSize,
                                            manifests, ArraySize(manifests)));

        for (uint32_t msec = 0; msec < LOOP_END_10000; msec += WAIT_TIME_5) {
            if (appStateListener.isClaimed(peer2Bus.GetUniqueName())) {
                break;
            }
            qcc::Sleep(WAIT_TIME_5);
        }


        ASSERT_EQ(PermissionConfigurator::ApplicationState::CLAIMED, appStateListener.stateMap[peer2Bus.GetUniqueName()]);

        // Switch to ECDHE_ECDSA-only
        EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener, nullptr, false, &managerConfigurationListener));
        EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener, nullptr, false, &peer1ConfigurationListener));
        EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener, nullptr, false, &peer2ConfigurationListener));
    }

    virtual void TearDown() {
        managerBus.Stop();
        managerBus.Join();

        peer1Bus.Stop();
        peer1Bus.Join();

        peer2Bus.Stop();
        peer2Bus.Join();

        delete managerAuthListener;
        delete peer1AuthListener;
        delete peer2AuthListener;
        delete peer3AuthListener;
    }

    void InstallMembershipOnManager() {
        // Get manager key
        KeyInfoNISTP256 managerKey;
        PermissionConfigurator& pcManager = managerBus.GetPermissionConfigurator();
        EXPECT_EQ(ER_OK, pcManager.GetSigningPublicKey(managerKey));

        String membershipSerial = "1";
        qcc::MembershipCertificate managerMembershipCertificate[1];
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert(membershipSerial,
                                                                        managerBus,
                                                                        managerBus.GetUniqueName(),
                                                                        managerKey.GetPublicKey(),
                                                                        managerGuid,
                                                                        true,
                                                                        3600,
                                                                        managerMembershipCertificate[0]
                                                                        ));
        SecurityApplicationProxy sapWithManagerBus(managerBus, managerBus.GetUniqueName().c_str());
        EXPECT_EQ(ER_OK, sapWithManagerBus.InstallMembership(managerMembershipCertificate, 1));
    }


    void InstallMembershipOnPeer1() {
        //Create peer1 key
        KeyInfoNISTP256 peer1Key;
        PermissionConfigurator& pcPeer1 = peer1Bus.GetPermissionConfigurator();
        EXPECT_EQ(ER_OK, pcPeer1.GetSigningPublicKey(peer1Key));

        String membershipSerial = "1";
        qcc::MembershipCertificate peer1MembershipCertificate[1];
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert(membershipSerial,
                                                                        managerBus,
                                                                        peer1Bus.GetUniqueName(),
                                                                        peer1Key.GetPublicKey(),
                                                                        managerGuid,
                                                                        false,
                                                                        3600,
                                                                        peer1MembershipCertificate[0]
                                                                        ));
        SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
        EXPECT_EQ(ER_OK, sapWithPeer1.InstallMembership(peer1MembershipCertificate, 1));
    }


    void InstallMembershipOnPeer2() {
        // Create peer2 key
        KeyInfoNISTP256 peer2Key;
        PermissionConfigurator& pcPeer2 = peer2Bus.GetPermissionConfigurator();
        EXPECT_EQ(ER_OK, pcPeer2.GetSigningPublicKey(peer2Key));

        String membershipSerial = "1";
        qcc::MembershipCertificate peer2MembershipCertificate[1];
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert(membershipSerial,
                                                                        managerBus,
                                                                        peer2Bus.GetUniqueName(),
                                                                        peer2Key.GetPublicKey(),
                                                                        managerGuid,
                                                                        false,
                                                                        3600,
                                                                        peer2MembershipCertificate[0]
                                                                        ));
        SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);
        EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(peer2MembershipCertificate, 1));
    }

    void SetManifestTemplate(BusAttachment& bus)
    {
        // All Inclusive manifest template
        PermissionPolicy::Rule::Member member[1];
        member[0].Set("*", PermissionPolicy::Rule::Member::NOT_SPECIFIED, PermissionPolicy::Rule::Member::ACTION_PROVIDE | PermissionPolicy::Rule::Member::ACTION_MODIFY | PermissionPolicy::Rule::Member::ACTION_OBSERVE);
        const size_t manifestSize = 1;
        PermissionPolicy::Rule manifestTemplate[manifestSize];
        manifestTemplate[0].SetObjPath("*");
        manifestTemplate[0].SetInterfaceName("*");
        manifestTemplate[0].SetMembers(1, member);
        EXPECT_EQ(ER_OK, bus.GetPermissionConfigurator().SetPermissionManifestTemplate(manifestTemplate, manifestSize));
    }

    static QStatus UpdatePolicyWithValuesFromDefaultPolicy(const PermissionPolicy& defaultPolicy,
                                                           PermissionPolicy& policy,
                                                           bool keepCAentry = true,
                                                           bool keepAdminGroupEntry = false,
                                                           bool keepInstallMembershipEntry = false);

    /*
     * this will create a Policy that will allow access to everything.
     * Many of the tests assume that a Bus is able to respond to method calls
     * The value of the policy is unimportant just that the bus is using security
     * and is responsive.
     *
     * DOES NOT add the CA entry from the default policy
     */
    static void CreatePermissivePolicy(PermissionPolicy& policy, uint32_t version);

    BusAttachment managerBus;
    BusAttachment peer1Bus;
    BusAttachment peer2Bus;
    BusAttachment peer3Bus;

    SessionPort managerSessionPort;
    SessionPort peer1SessionPort;
    SessionPort peer2SessionPort;

    SecurityManagementTestSessionPortListener managerSessionPortListener;
    SecurityManagementTestSessionPortListener peer1SessionPortListener;
    SecurityManagementTestSessionPortListener peer2SessionPortListener;

    SessionId managerToManagerSessionId;
    SessionId managerToPeer1SessionId;
    SessionId managerToPeer2SessionId;

    InMemoryKeyStoreListener managerKeyStoreListener;
    InMemoryKeyStoreListener peer1KeyStoreListener;
    InMemoryKeyStoreListener peer2KeyStoreListener;
    InMemoryKeyStoreListener peer3KeyStoreListener;

    String interface;
    const char* interfaceName;
    DefaultECDHEAuthListener* managerAuthListener;
    DefaultECDHEAuthListener* peer1AuthListener;
    DefaultECDHEAuthListener* peer2AuthListener;
    DefaultECDHEAuthListener* peer3AuthListener;

    SecurityManagement_ApplicationStateListener appStateListener;

    //Random GUID used for the SecurityManager
    GUID128 managerGuid;

    SecurityManagementTestConfigurationListener managerConfigurationListener;
    SecurityManagementTestConfigurationListener peer1ConfigurationListener;
    SecurityManagementTestConfigurationListener peer2ConfigurationListener;
    SecurityManagementTestConfigurationListener peer3ConfigurationListener;
};

QStatus SecurityManagementPolicyTest::UpdatePolicyWithValuesFromDefaultPolicy(const PermissionPolicy& defaultPolicy,
                                                                              PermissionPolicy& policy,
                                                                              bool keepCAentry,
                                                                              bool keepAdminGroupEntry,
                                                                              bool keepInstallMembershipEntry) {

    size_t count = policy.GetAclsSize();
    if (keepCAentry) {
        ++count;
    }
    if (keepAdminGroupEntry) {
        ++count;
    }
    if (keepInstallMembershipEntry) {
        ++count;
    }

    PermissionPolicy::Acl* acls = new PermissionPolicy::Acl[count];
    size_t idx = 0;

    for (size_t cnt = 0; cnt < defaultPolicy.GetAclsSize(); ++cnt) {
        if (defaultPolicy.GetAcls()[cnt].GetPeersSize() > 0) {
            if (defaultPolicy.GetAcls()[cnt].GetPeers()[0].GetType() == PermissionPolicy::Peer::PEER_FROM_CERTIFICATE_AUTHORITY) {
                if (keepCAentry) {
                    acls[idx++] = defaultPolicy.GetAcls()[cnt];
                }
            } else if (defaultPolicy.GetAcls()[cnt].GetPeers()[0].GetType() == PermissionPolicy::Peer::PEER_WITH_MEMBERSHIP) {
                if (keepAdminGroupEntry) {
                    acls[idx++] = defaultPolicy.GetAcls()[cnt];
                }
            } else if (defaultPolicy.GetAcls()[cnt].GetPeers()[0].GetType() == PermissionPolicy::Peer::PEER_WITH_PUBLIC_KEY) {
                if (keepInstallMembershipEntry) {
                    acls[idx++] = defaultPolicy.GetAcls()[cnt];
                }
            }
        }

    }

    for (size_t cnt = 0; cnt < policy.GetAclsSize(); ++cnt) {
        QCC_ASSERT(idx <= count);
        acls[idx++] = policy.GetAcls()[cnt];
    }

    policy.SetAcls(count, acls);
    delete [] acls;
    return ER_OK;
}

void SecurityManagementPolicyTest::CreatePermissivePolicy(PermissionPolicy& policy, uint32_t version) {
    policy.SetVersion(version);
    {
        PermissionPolicy::Acl acls[1];
        {
            PermissionPolicy::Peer peers[1];
            peers[0].SetType(PermissionPolicy::Peer::PEER_ALL);
            acls[0].SetPeers(1, peers);
        }
        {
            PermissionPolicy::Rule rules[1];
            rules[0].SetObjPath("*");
            rules[0].SetInterfaceName("*");
            {
                PermissionPolicy::Rule::Member members[1];
                members[0].Set("*",
                               PermissionPolicy::Rule::Member::NOT_SPECIFIED,
                               PermissionPolicy::Rule::Member::ACTION_PROVIDE |
                               PermissionPolicy::Rule::Member::ACTION_MODIFY |
                               PermissionPolicy::Rule::Member::ACTION_OBSERVE);
                rules[0].SetMembers(1, members);
            }
            acls[0].SetRules(1, rules);
        }
        policy.SetAcls(1, acls);
    }
}

/*
 * Purpose:
 * Latest Policy to be installed should have a serial number greater than the
 * previous policy's serial number. Else, the previous policy should not be deleted.
 *
 * SetUp:
 * manager claims the peer1
 * manager creates a policy  (policy 1):
 * Serial number: 1234
 * ACL: ANY_TRUSTED
 * Rule1: Object Path=*, Interface=*, Member Name=*, Type=Method, Action mask:  PROVIDE
 *
 * manager calls UpdatePolicy on peer1.
 * manager calls GetProperty ("policy") on the peer1
 *
 * manager creates another policy  (policy 2):
 * Serial number: 1200
 * ACL: ALL
 * Rule1: Object Path=/abc, Interface=*, Member Name=*, Type=Method, Action mask:  PROVIDE
 * manager calls UpdatePolicy on peer1
 * manager calls GetProperty ("policy") on peer1.
 *
 * Verify:
 * UpdatePolicy (policy1) should succeed.
 * GetProperty ("Policy") should fetch policy 1.
 * Update policy ("Policy2") should fail with ER_POLICY_NOT_NEWER.
 * GetProperty("Policy") should still fetch policy1.
 */
TEST_F(SecurityManagementPolicyTest, UpdatePolicy_fails_if_version_not_newer)
{
    InstallMembershipOnManager();
    InstallMembershipOnPeer1();

    /*
     * creates a policy  (policy 1):
     * Serial number: 1234
     * ACL: ANY_TRUSTED
     * Rule1: Object Path=*, Interface=*, Member Name=*, Type=Method, Action mask:  PROVIDE
     */
    PermissionPolicy policy1;
    policy1.SetVersion(1234);
    {
        PermissionPolicy::Acl acls[1];
        {
            PermissionPolicy::Peer peers[1];
            peers[0].SetType(PermissionPolicy::Peer::PEER_ANY_TRUSTED);
            acls[0].SetPeers(1, peers);
        }
        {
            PermissionPolicy::Rule rules[1];
            rules[0].SetObjPath("*");
            rules[0].SetInterfaceName("*");
            {
                PermissionPolicy::Rule::Member members[1];
                members[0].Set("*",
                               PermissionPolicy::Rule::Member::METHOD_CALL,
                               PermissionPolicy::Rule::Member::ACTION_PROVIDE);
                rules[0].SetMembers(1, members);
            }
            acls[0].SetRules(1, rules);
        }
        policy1.SetAcls(1, acls);
    }

    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);

    {
        PermissionPolicy peer1DefaultPolicy;
        EXPECT_EQ(ER_OK, sapWithPeer1.GetDefaultPolicy(peer1DefaultPolicy));
        UpdatePolicyWithValuesFromDefaultPolicy(peer1DefaultPolicy, policy1, true, true);
    }

    EXPECT_EQ(ER_OK, sapWithPeer1.UpdatePolicy(policy1));
    EXPECT_EQ(ER_OK, sapWithPeer1.SecureConnection(true));

    PermissionPolicy fetchedPolicy;
    EXPECT_EQ(ER_OK, sapWithPeer1.GetPolicy(fetchedPolicy));
    EXPECT_EQ(static_cast<uint32_t>(1234), fetchedPolicy.GetVersion());
    EXPECT_EQ(policy1.GetVersion(), fetchedPolicy.GetVersion());
    EXPECT_EQ(policy1, fetchedPolicy);

    /*
     * creates another policy  (policy 2):
     * Serial number: 1200
     * ACL: ALL
     * Rule1: Object Path=/abc, Interface=*, Member Name=*, Type=Method, Action mask:  PROVIDE
     * manager calls UpdatePolicy on peer1
     * manager calls GetProperty ("policy") on peer1.
     */
    PermissionPolicy policy2;
    policy2.SetVersion(1200);
    {
        PermissionPolicy::Acl acls[1];
        {
            PermissionPolicy::Peer peers[1];
            peers[0].SetType(PermissionPolicy::Peer::PEER_ALL);
            acls[0].SetPeers(1, peers);
        }
        {
            PermissionPolicy::Rule rules[1];
            rules[0].SetObjPath("/abc");
            rules[0].SetInterfaceName("*");
            {
                PermissionPolicy::Rule::Member members[1];
                members[0].Set("*",
                               PermissionPolicy::Rule::Member::METHOD_CALL,
                               PermissionPolicy::Rule::Member::ACTION_PROVIDE);
                rules[0].SetMembers(1, members);
            }
            acls[0].SetRules(1, rules);
        }
        policy2.SetAcls(1, acls);
    }


    EXPECT_EQ(ER_POLICY_NOT_NEWER, sapWithPeer1.UpdatePolicy(policy2));

    {
        PermissionPolicy peer1DefaultPolicy;
        EXPECT_EQ(ER_OK, sapWithPeer1.GetDefaultPolicy(peer1DefaultPolicy));
        UpdatePolicyWithValuesFromDefaultPolicy(peer1DefaultPolicy, policy2, true, true);
    }

    EXPECT_EQ(ER_OK, sapWithPeer1.GetPolicy(fetchedPolicy));
    EXPECT_EQ(policy1.GetVersion(), fetchedPolicy.GetVersion());
    EXPECT_EQ(policy1, fetchedPolicy);
}

/*
 * Purpose:
 * New policy installed should override the older policy.
 *
 * SetUp:
 * manager claims the peer1
 *
 * manager creates a policy  (policy 1):
 * Serial number: 1234
 * ACL: ANY_TRUSTED
 * Rule1: Object Path=*, Interface=*, Member Name=*, Type=Method, Action mask:  PROVIDE
 *
 * manager calls UpdatePolicy on peer1
 * manager calls GetProperty ("policy") on peer1
 *
 * manager creates another policy  (policy 2):
 * Serial number: 1235
 * ACL: ALL
 * Rule1: Object Path=/abc, Interface=*, Member Name=*, Type=Method, Action mask:  PROVIDE
 *
 * manager calls UpdatePolicy on peer1
 * manager calls GetProperty ("policy") on peer1
 *
 * Verify:
 * UpdatePolicy (policy1) should succeed.
 * GetProperty ("Policy") should fetch policy 1.
 * Update policy ("Policy2") should succeed.
 * GetProperty("Policy") should still fetch policy2.
 */
TEST_F(SecurityManagementPolicyTest, UpdatePolicy_new_policy_should_override_older_policy)
{

    InstallMembershipOnManager();
    InstallMembershipOnPeer1();

    /*
     * manager creates a policy  (policy 1):
     * Serial number: 1234
     * ACL: ANY_TRUSTED
     * Rule1: Object Path=*, Interface=*, Member Name=*, Type=Method, Action mask:  PROVIDE
     */
    PermissionPolicy policy1;
    policy1.SetVersion(1234);
    {
        PermissionPolicy::Acl acls[1];
        {
            PermissionPolicy::Peer peers[1];
            peers[0].SetType(PermissionPolicy::Peer::PEER_ANY_TRUSTED);
            acls[0].SetPeers(1, peers);
        }
        {
            PermissionPolicy::Rule rules[1];
            rules[0].SetObjPath("*");
            rules[0].SetInterfaceName("*");
            {
                PermissionPolicy::Rule::Member members[1];
                members[0].Set("*",
                               PermissionPolicy::Rule::Member::METHOD_CALL,
                               PermissionPolicy::Rule::Member::ACTION_PROVIDE);
                rules[0].SetMembers(1, members);
            }
            acls[0].SetRules(1, rules);
        }
        policy1.SetAcls(1, acls);
    }

    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);

    {
        PermissionPolicy peer1DefaultPolicy;
        EXPECT_EQ(ER_OK, sapWithPeer1.GetDefaultPolicy(peer1DefaultPolicy));
        UpdatePolicyWithValuesFromDefaultPolicy(peer1DefaultPolicy, policy1, true, true);
    }

    EXPECT_EQ(ER_OK, sapWithPeer1.UpdatePolicy(policy1));
    EXPECT_EQ(ER_OK, sapWithPeer1.SecureConnection(true));

    PermissionPolicy fetchedPolicy;
    EXPECT_EQ(ER_OK, sapWithPeer1.GetPolicy(fetchedPolicy));

    EXPECT_EQ(policy1.GetVersion(), fetchedPolicy.GetVersion());
    EXPECT_EQ(policy1, fetchedPolicy);

    /*
     * manager creates another policy  (policy 2):
     * Serial number: 1235
     * ACL: ALL
     * Rule1: Object Path=/abc, Interface=*, Member Name=*, Type=Method, Action mask:  PROVIDE
     */
    PermissionPolicy policy2;
    policy2.SetVersion(1235);
    {
        PermissionPolicy::Acl acls[1];
        {
            PermissionPolicy::Peer peers[1];
            peers[0].SetType(PermissionPolicy::Peer::PEER_ALL);
            acls[0].SetPeers(1, peers);
        }
        {
            PermissionPolicy::Rule rules[1];
            rules[0].SetObjPath("/abc");
            rules[0].SetInterfaceName("*");
            {
                PermissionPolicy::Rule::Member members[1];
                members[0].Set("*",
                               PermissionPolicy::Rule::Member::METHOD_CALL,
                               PermissionPolicy::Rule::Member::ACTION_PROVIDE);
                rules[0].SetMembers(1, members);
            }
            acls[0].SetRules(1, rules);
        }
        policy2.SetAcls(1, acls);
    }

    {
        PermissionPolicy peer1DefaultPolicy;
        EXPECT_EQ(ER_OK, sapWithPeer1.GetDefaultPolicy(peer1DefaultPolicy));
        UpdatePolicyWithValuesFromDefaultPolicy(peer1DefaultPolicy, policy2, true, true);
    }

    EXPECT_EQ(ER_OK, sapWithPeer1.UpdatePolicy(policy2));
    EXPECT_EQ(ER_OK, sapWithPeer1.SecureConnection(true));

    PermissionPolicy fetchedPolicy2;

    EXPECT_EQ(ER_OK, sapWithPeer1.GetPolicy(fetchedPolicy2));
    EXPECT_NE(policy1, fetchedPolicy2);
    EXPECT_EQ(policy2.GetVersion(), fetchedPolicy2.GetVersion());
    EXPECT_EQ(policy2, fetchedPolicy2);
}

/* these keys were generated by the unit test common/unit_test/CertificateECCTest.GenSelfSignECCX509CertForBBservice */
static const char ecdsaPrivateKeyPEM[] = {
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MDECAQEEII9MndKAfsYuLIsINFNkTmTMslzcYglHcVF/+l2dg2dxoAoGCCqGSM49\n"
    "AwEH\n"
    "-----END EC PRIVATE KEY-----"
};

static const char ecdsaCertChainX509PEM[] = {
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBtTCCAVugAwIBAgIHMTAxMDEwMTAKBggqhkjOPQQDAjBCMRUwEwYDVQQLDAxv\n"
    "cmdhbml6YXRpb24xKTAnBgNVBAMMIDI2MDM2YzFlMDM1ZjgzYTczNWQ1YTZmODVi\n"
    "YjhmYjE1MB4XDTE2MDIyNzAwMjQyNFoXDTI2MDIyNDAwMjQyNFowQjEVMBMGA1UE\n"
    "CwwMb3JnYW5pemF0aW9uMSkwJwYDVQQDDCBiNTMzMzExZDg2NDhkY2MwNTQ3NzM2\n"
    "ZDUwOTRiYjYyMDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABEET2YJ7f0tvyyPj\n"
    "mx9WLA/0IWfKsp/PmpBH3h2VcJgKVinRDi5RTn5aBV6GkYCT2S/pMkwyqvv6ZbRP\n"
    "sYwM402jPDA6MAwGA1UdEwQFMAMBAf8wFQYDVR0lBA4wDAYKKwYBBAGC3nwBATAT\n"
    "BgNVHSMEDDAKoAhHNsLWWLZ/4zAKBggqhkjOPQQDAgNIADBFAiBjfRMGrHQ49Ys7\n"
    "tjgN8u+4AgraJ4ep5PbZTsdQUAqptQIhAKjAYghpuu95Wfg7GSNPShtZOm/FfB3I\n"
    "sr1PNKFcqHcL\n"
    "-----END CERTIFICATE-----"
};

class SecurityManagementPolicy2AuthListener : public AuthListener {

  public:
    SecurityManagementPolicy2AuthListener() : authenticationSuccessfull(false) {
    }

    QStatus RequestCredentialsAsync(const char* authMechanism, const char* authPeer, uint16_t authCount, const char* userId, uint16_t credMask, void* context)
    {
        QCC_UNUSED(userId);
        QCC_UNUSED(authCount);
        QCC_UNUSED(authPeer);
        Credentials creds;
        if (strcmp(authMechanism, "ALLJOYN_ECDHE_ECDSA") == 0) {
            if ((credMask& AuthListener::CRED_PRIVATE_KEY) == AuthListener::CRED_PRIVATE_KEY) {
                String pk(ecdsaPrivateKeyPEM, strlen(ecdsaPrivateKeyPEM));
                creds.SetPrivateKey(pk);
                //printf("AuthListener::RequestCredentials for key exchange %s sends DSA private key %s\n", authMechanism, pk.c_str());
            }
            if ((credMask& AuthListener::CRED_CERT_CHAIN) == AuthListener::CRED_CERT_CHAIN) {
                String cert(ecdsaCertChainX509PEM, strlen(ecdsaCertChainX509PEM));
                creds.SetCertChain(cert);
                //printf("AuthListener::RequestCredentials for key exchange %s sends DSA public cert %s\n", authMechanism, cert.c_str());
            }
            return RequestCredentialsResponse(context, true, creds);
        }
        return RequestCredentialsResponse(context, false, creds);
    }
    QStatus VerifyCredentialsAsync(const char* authMechanism, const char* authPeer, const Credentials& creds, void* context) {
        QCC_UNUSED(authPeer);
        if (strcmp(authMechanism, "ALLJOYN_ECDHE_ECDSA") == 0) {
            if (creds.IsSet(AuthListener::CRED_CERT_CHAIN)) {
                //printf("Verify\n%s\n", creds.GetCertChain().c_str());
                return VerifyCredentialsResponse(context, true);
            }
        }
        return VerifyCredentialsResponse(context, false);
    }

    void AuthenticationComplete(const char* authMechanism, const char* authPeer, bool success) {
        QCC_UNUSED(authMechanism);
        QCC_UNUSED(authPeer);
        QCC_UNUSED(success);
        if (success) {
            authenticationSuccessfull = true;
        }
    }

    void SecurityViolation(QStatus status, const Message& msg) {
        QCC_UNUSED(status);
        QCC_UNUSED(msg);
    }
    bool authenticationSuccessfull;

};

TEST_F(SecurityManagementPolicyTest, Update_identity_fails_on_invalid_icc_chain)
{
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    uint8_t intermediateCN[] = { 5, 6, 7, 8 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    //Create the CA cert
    qcc::IdentityCertificate caCert;
    caCert.SetSerial((uint8_t*)"5678", 5);
    caCert.SetIssuerCN(managerCN, 4);
    caCert.SetSubjectCN(managerCN, 4);
    CertificateX509::ValidPeriod validityCA;
    validityCA.validFrom = 1427404154;
    validityCA.validTo = 1427404154 + 630720000;
    caCert.SetValidity(&validityCA);

    KeyInfoNISTP256 managerPublicKey;
    PermissionConfigurator& permissionConfigurator = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, permissionConfigurator.GetSigningPublicKey(managerPublicKey));

    caCert.SetSubjectPublicKey(managerPublicKey.GetPublicKey());
    caCert.SetAlias("ca-cert-alias");
    caCert.SetCA(true);

    //sign the ca cert
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(caCert));

    // Create the intermediate certificate using peer1
    qcc::IdentityCertificate peer1Cert;
    peer1Cert.SetSerial((uint8_t*)"1234", 5);
    peer1Cert.SetIssuerCN(managerCN, 4);
    peer1Cert.SetSubjectCN(intermediateCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer1Cert.SetValidity(&validity);

    ECCPublicKey peer1PublicKey;
    sapWithPeer1.GetEccPublicKey(peer1PublicKey);

    peer1Cert.SetSubjectPublicKey(&peer1PublicKey);
    peer1Cert.SetAlias("intermediate-cert-alias");
    peer1Cert.SetCA(true);

    //We intentionally skip signing the leaf cert
    PermissionConfigurator& peer3PermissionConfigurator = peer3Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer3PermissionConfigurator.SignCertificate(peer1Cert));

    // Create the leaf certificate using peer2
    qcc::IdentityCertificate peer2Cert;
    peer2Cert.SetSerial((uint8_t*)"1234", 5);
    peer2Cert.SetIssuerCN(intermediateCN, 4);
    peer2Cert.SetSubjectCN(leafCN, 4);
    peer2Cert.SetValidity(&validity);

    ECCPublicKey peer2PublicKey;
    sapWithPeer2.GetEccPublicKey(peer2PublicKey);

    peer2Cert.SetSubjectPublicKey(&peer2PublicKey);
    peer2Cert.SetAlias("peer2-cert-alias");
    peer2Cert.SetCA(true);

    //sign the leaf cert
    PermissionConfigurator& peer1PermissionConfigurator = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PermissionConfigurator.SignCertificate(peer2Cert));

    //We need identityCert chain CA1->Peer2
    const size_t certChainSize = 3;
    IdentityCertificate identityCertChain[certChainSize];
    identityCertChain[0] = peer2Cert;
    identityCertChain[1] = peer1Cert;
    identityCertChain[2] = caCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener, nullptr, false, &managerConfigurationListener));
    EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener, nullptr, false, &peer1ConfigurationListener));

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(peer1Bus, identityCertChain[0], manifests[0]));
    // Call UpdateIdentity to install the cert chain
    EXPECT_EQ(ER_INVALID_CERTIFICATE, sapWithPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)))
        << "Failed to update Identity cert or manifest ";
}

TEST_F(SecurityManagementPolicyTest, Update_identity_fails_on_intermediate_ca_flag_false)
{
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    uint8_t intermediateCN[] = { 5, 6, 7, 8 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    //Create the CA cert
    qcc::IdentityCertificate caCert;
    caCert.SetSerial((uint8_t*)"5678", 5);
    caCert.SetIssuerCN(managerCN, 4);
    caCert.SetSubjectCN(managerCN, 4);
    CertificateX509::ValidPeriod validityCA;
    validityCA.validFrom = 1427404154;
    validityCA.validTo = 1427404154 + 630720000;
    caCert.SetValidity(&validityCA);

    KeyInfoNISTP256 managerPublicKey;
    PermissionConfigurator& permissionConfigurator = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, permissionConfigurator.GetSigningPublicKey(managerPublicKey));

    caCert.SetSubjectPublicKey(managerPublicKey.GetPublicKey());
    caCert.SetAlias("ca-cert-alias");
    caCert.SetCA(true);

    //sign the ca cert
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(caCert));

    // Create the intermediate certificate using peer1
    qcc::IdentityCertificate peer1Cert;
    peer1Cert.SetSerial((uint8_t*)"1234", 5);
    peer1Cert.SetIssuerCN(managerCN, 4);
    peer1Cert.SetSubjectCN(intermediateCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer1Cert.SetValidity(&validity);

    ECCPublicKey peer1PublicKey;
    sapWithPeer1.GetEccPublicKey(peer1PublicKey);

    peer1Cert.SetSubjectPublicKey(&peer1PublicKey);
    peer1Cert.SetAlias("intermediate-cert-alias");
    peer1Cert.SetCA(false);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(peer1Cert));

    // Create the leaf certificate using peer2
    qcc::IdentityCertificate peer2Cert;
    peer2Cert.SetSerial((uint8_t*)"1234", 5);
    peer2Cert.SetIssuerCN(intermediateCN, 4);
    peer2Cert.SetSubjectCN(leafCN, 4);
    peer2Cert.SetValidity(&validity);

    ECCPublicKey peer2PublicKey;
    sapWithPeer2.GetEccPublicKey(peer2PublicKey);

    peer2Cert.SetSubjectPublicKey(&peer2PublicKey);
    peer2Cert.SetAlias("peer2-cert-alias");
    peer2Cert.SetCA(true);

    //sign the leaf cert
    PermissionConfigurator& peer1PermissionConfigurator = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PermissionConfigurator.SignCertificate(peer2Cert));

    //We need identityCert chain CA1->Peer2
    const size_t certChainSize = 3;
    IdentityCertificate identityCertChain[certChainSize];
    identityCertChain[0] = peer2Cert;
    identityCertChain[1] = peer1Cert;
    identityCertChain[2] = caCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener, nullptr, false, &managerConfigurationListener));
    EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener, nullptr, false, &peer1ConfigurationListener));

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(peer1Bus, identityCertChain[0], manifests[0]));
    // Call UpdateIdentity to install the cert chain
    EXPECT_EQ(ER_INVALID_CERTIFICATE, sapWithPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)))
        << "Failed to update Identity cert or manifest ";
}


TEST_F(SecurityManagementPolicyTest, Update_identity_fails_on_different_subject_leaf_node)
{
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    uint8_t intermediateCN[] = { 5, 6, 7, 8 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    //Create the CA cert
    qcc::IdentityCertificate caCert;
    caCert.SetSerial((uint8_t*)"5678", 5);
    caCert.SetIssuerCN(managerCN, 4);
    caCert.SetSubjectCN(managerCN, 4);
    CertificateX509::ValidPeriod validityCA;
    validityCA.validFrom = 1427404154;
    validityCA.validTo = 1427404154 + 630720000;
    caCert.SetValidity(&validityCA);

    KeyInfoNISTP256 managerPublicKey;
    PermissionConfigurator& permissionConfigurator = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, permissionConfigurator.GetSigningPublicKey(managerPublicKey));

    caCert.SetSubjectPublicKey(managerPublicKey.GetPublicKey());
    caCert.SetAlias("ca-cert-alias");
    caCert.SetCA(true);

    //sign the ca cert
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(caCert));

    // Create the intermediate certificate using peer1
    qcc::IdentityCertificate peer1Cert;
    peer1Cert.SetSerial((uint8_t*)"1234", 5);
    peer1Cert.SetIssuerCN(managerCN, 4);
    peer1Cert.SetSubjectCN(intermediateCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer1Cert.SetValidity(&validity);

    ECCPublicKey peer1PublicKey;
    sapWithPeer1.GetEccPublicKey(peer1PublicKey);

    peer1Cert.SetSubjectPublicKey(&peer1PublicKey);
    peer1Cert.SetAlias("intermediate-cert-alias");
    peer1Cert.SetCA(false);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(peer1Cert));

    // Create the leaf certificate using peer2
    qcc::IdentityCertificate peer2Cert;
    peer2Cert.SetSerial((uint8_t*)"1234", 5);
    peer2Cert.SetIssuerCN(intermediateCN, 4);
    peer2Cert.SetSubjectCN(leafCN, 4);
    peer2Cert.SetValidity(&validity);

    // We are intentionally making the leaf certificate public key different
    peer2Cert.SetSubjectPublicKey(&peer1PublicKey);
    peer2Cert.SetAlias("peer2-cert-alias");
    peer2Cert.SetCA(true);

    //sign the leaf cert
    PermissionConfigurator& peer1PermissionConfigurator = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PermissionConfigurator.SignCertificate(peer2Cert));

    //We need identityCert chain CA1->Peer2
    const size_t certChainSize = 3;
    IdentityCertificate identityCertChain[certChainSize];
    identityCertChain[0] = peer2Cert;
    identityCertChain[1] = peer1Cert;
    identityCertChain[2] = caCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener, nullptr, false, &managerConfigurationListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener, nullptr, false, &peer2ConfigurationListener));

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(peer1Bus, identityCertChain[0], manifests[0]));
    // Call UpdateIdentity to install the cert chain
    EXPECT_EQ(ER_INVALID_CERTIFICATE, sapWithPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)))
        << "Failed to update Identity cert or manifest ";
}

TEST_F(SecurityManagementPolicyTest, Update_identity_succeeds_on_long_icc)
{
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    uint8_t intermediateCN[] = { 5, 6, 7, 8 };
    uint8_t intermediate2CN[] = { 4, 3, 2, 1 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    //Create the CA cert
    qcc::IdentityCertificate caCert;
    caCert.SetSerial((uint8_t*)"1234", 5);
    caCert.SetIssuerCN(managerCN, 4);
    caCert.SetSubjectCN(managerCN, 4);
    CertificateX509::ValidPeriod validityCA;
    validityCA.validFrom = qcc::GetEpochTimestamp() / 1000;
    validityCA.validTo = validityCA.validFrom + TEN_MINS;
    caCert.SetValidity(&validityCA);

    KeyInfoNISTP256 managerPublicKey;
    PermissionConfigurator& permissionConfigurator = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, permissionConfigurator.GetSigningPublicKey(managerPublicKey));

    caCert.SetSubjectPublicKey(managerPublicKey.GetPublicKey());
    caCert.SetAlias("ca-cert-alias");
    caCert.SetCA(true);

    //sign the ca cert
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(caCert));

    // Create the intermediate certificate using peer1
    qcc::IdentityCertificate peer1Cert;
    peer1Cert.SetSerial((uint8_t*)"2345", 5);
    peer1Cert.SetIssuerCN(managerCN, 4);
    peer1Cert.SetSubjectCN(intermediateCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer1Cert.SetValidity(&validity);

    ECCPublicKey peer1PublicKey;
    sapWithPeer1.GetEccPublicKey(peer1PublicKey);

    peer1Cert.SetSubjectPublicKey(&peer1PublicKey);
    peer1Cert.SetAlias("intermediate-cert-alias");
    peer1Cert.SetCA(true);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(peer1Cert));

    // Create third intermediate CA
    KeyInfoNISTP256 peer3PublicKey;
    PermissionConfigurator& peer3PermissionConfigurator = peer3Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer3PermissionConfigurator.GetSigningPublicKey(peer3PublicKey));

    qcc::IdentityCertificate intermediateCACert;
    intermediateCACert.SetSerial((uint8_t*)"1234", 5);
    intermediateCACert.SetIssuerCN(intermediateCN, 4);
    intermediateCACert.SetSubjectCN(intermediate2CN, 4);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    intermediateCACert.SetValidity(&validity);

    intermediateCACert.SetSubjectPublicKey(peer3PublicKey.GetPublicKey());
    intermediateCACert.SetAlias("intermediate-ca-cert-alias");
    intermediateCACert.SetCA(true);

    //sign the intermediate 2 cert
    PermissionConfigurator& peer1PermissionConfigurator = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PermissionConfigurator.SignCertificate(intermediateCACert));

    // Create the leaf certificate using peer2
    qcc::IdentityCertificate peer2Cert;
    peer2Cert.SetSerial((uint8_t*)"1234", 5);
    peer2Cert.SetIssuerCN(intermediate2CN, 4);
    peer2Cert.SetSubjectCN(leafCN, 4);
    peer2Cert.SetValidity(&validity);

    ECCPublicKey peer2PublicKey;
    sapWithPeer2.GetEccPublicKey(peer2PublicKey);
    peer2Cert.SetSubjectPublicKey(&peer2PublicKey);
    peer2Cert.SetAlias("peer2-cert-alias");
    peer2Cert.SetCA(false);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, peer3PermissionConfigurator.SignCertificate(peer2Cert));

    const size_t certChainSize = 4;
    IdentityCertificate identityCertChain[certChainSize];
    identityCertChain[0] = peer2Cert;
    identityCertChain[1] = intermediateCACert;
    identityCertChain[2] = peer1Cert;
    identityCertChain[3] = caCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));
    EXPECT_EQ(ER_OK, peer3Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer3AuthListener));

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(peer3Bus, identityCertChain[0], manifests[0]));
    // Call UpdateIdentity to install the cert chain
    EXPECT_EQ(ER_OK, sapWithPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)))
        << "Failed to update Identity cert or manifest ";
}

TEST_F(SecurityManagementPolicyTest, Update_identity_single_icc_any_sign)
{
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t leafCN[] = { 9, 0, 1, 2 };

    KeyInfoNISTP256 peer1PublicKey;
    PermissionConfigurator& peer1PermissionConfigurator = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PermissionConfigurator.GetSigningPublicKey(peer1PublicKey));

    // Create the leaf certificate using peer2
    qcc::IdentityCertificate peer2Cert;
    peer2Cert.SetSerial((uint8_t*)"1234", 5);
    peer2Cert.SetIssuerCN(leafCN, 4);
    peer2Cert.SetSubjectCN(leafCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer2Cert.SetValidity(&validity);

    ECCPublicKey peer2PublicKey;
    sapWithPeer2.GetEccPublicKey(peer2PublicKey);
    peer2Cert.SetSubjectPublicKey(&peer2PublicKey);
    peer2Cert.SetAlias("peer2-cert-alias");
    peer2Cert.SetCA(true);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, peer1PermissionConfigurator.SignCertificate(peer2Cert));

    //We need identityCert chain CA1->Peer2
    const size_t certChainSize = 1;
    IdentityCertificate identityCertChain[certChainSize];
    identityCertChain[0] = peer2Cert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(peer1Bus, identityCertChain[0], manifests[0]));
    // Call UpdateIdentity to install the cert chain.
    EXPECT_EQ(ER_OK, sapWithPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)))
        << "Failed to update Identity cert or manifest ";
}

TEST_F(SecurityManagementPolicyTest, DISABLED_install_membership_fails_with_invalid_public_key)
{
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    KeyInfoNISTP256 managerPublicKey;
    PermissionConfigurator& permissionConfigurator = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, permissionConfigurator.GetSigningPublicKey(managerPublicKey));

    KeyInfoNISTP256 peer1PublicKey;
    PermissionConfigurator& peer1PermissionConfigurator = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PermissionConfigurator.GetSigningPublicKey(peer1PublicKey));


    qcc::MembershipCertificate membershipCertificate[2];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("1",
                                                                    managerBus,
                                                                    managerBus.GetUniqueName(),
                                                                    managerPublicKey.GetPublicKey(),
                                                                    managerGuid,
                                                                    true,
                                                                    3600,
                                                                    membershipCertificate[1]
                                                                    ));

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("2",
                                                                    managerBus,
                                                                    peer1Bus.GetUniqueName(),
                                                                    peer1PublicKey.GetPublicKey(),
                                                                    managerGuid,
                                                                    false,
                                                                    3600,
                                                                    membershipCertificate[0]
                                                                    ));


    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));

    // Call UpdateIdentity to install the cert chain
    EXPECT_EQ(ER_INVALID_CERTIFICATE, sapWithPeer2.InstallMembership(membershipCertificate, 2)) << "Failed to install membership ";
}

TEST_F(SecurityManagementPolicyTest, install_membership_fails_with_same_cert_serial)
{
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));

    KeyInfoNISTP256 peer2PublicKey;
    PermissionConfigurator& peer2PermissionConfigurator = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer2PermissionConfigurator.GetSigningPublicKey(peer2PublicKey));

    qcc::MembershipCertificate membershipCertificate[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("1",
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2PublicKey.GetPublicKey(),
                                                                    managerGuid,
                                                                    true,
                                                                    3600,
                                                                    membershipCertificate[0]
                                                                    ));


    // Call InstallMembership
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(membershipCertificate, 1)) << "Failed to install membership ";


    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("1",
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2PublicKey.GetPublicKey(),
                                                                    managerGuid,
                                                                    true,
                                                                    3600,
                                                                    membershipCertificate[0]
                                                                    ));

    // Call InstallMembership
    EXPECT_EQ(ER_DUPLICATE_CERTIFICATE, sapWithPeer2.InstallMembership(membershipCertificate, 1)) << "Failed to install membership ";
}

TEST_F(SecurityManagementPolicyTest, DISABLED_remove_membership_succeeds)
{
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);
    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t issuerCN[] = { 1, 2, 3, 4 };
    uint8_t leafCN[] = { 5, 6, 7, 8 };

    // Create the membership certificate from the first issuer
    qcc::MembershipCertificate memCert;
    memCert.SetSerial((uint8_t*)"1234", 5);
    memCert.SetIssuerCN(issuerCN, 4);
    memCert.SetSubjectCN(leafCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    memCert.SetValidity(&validity);

    ECCPublicKey peer2PublicKey;
    sapWithPeer2.GetEccPublicKey(peer2PublicKey);
    memCert.SetSubjectPublicKey(&peer2PublicKey);
    memCert.SetCA(true);
    GUID128 asgaGUID;
    memCert.SetGuild(asgaGUID);

    //sign the leaf cert
    PermissionConfigurator& permissionConfigurator = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(memCert));

    // Call InstallMembership
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(&memCert, 1)) << "Failed to install membership ";

    // Create the membership certificate from the second issuer Peer 1
    qcc::MembershipCertificate memCert2;
    memCert2.SetSerial((uint8_t*)"5678", 5);
    memCert2.SetIssuerCN(issuerCN, 4);
    memCert2.SetSubjectCN(leafCN, 4);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    memCert2.SetValidity(&validity);
    memCert2.SetSubjectPublicKey(&peer2PublicKey);
    memCert2.SetCA(true);
    memCert.SetGuild(asgaGUID);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, permissionConfigurator.SignCertificate(memCert2));

    // Call InstallMembership
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(&memCert2, 1)) << "Failed to install membership ";

    // Call GetProperty("MembershipSummaries"). This call should show 2 membership certificates
    MsgArg arg;
    EXPECT_EQ(ER_OK, sapWithPeer2.GetMembershipSummaries(arg)) << "GetMembershipSummaries failed.";

    size_t count = arg.v_array.GetNumElements();
    EXPECT_EQ((uint32_t)2, count);
    String*serials = new String[count];
    KeyInfoNISTP256* keyInfos = new KeyInfoNISTP256[count];
    EXPECT_EQ(ER_OK, sapWithPeer2.MsgArgToCertificateIds(arg, serials, keyInfos, count));

    String serial0("1234");
    String serial1("5678");
    // Compare the serial  in the certificates just retrieved
    // Membership certs are stored as a non-deterministic set so the order can
    // change. We just want to make sure both certificates are returned. The
    // only time order will remain the same is if the certificates are in a
    // certificate chain.

    if (serials[0].compare(serial0) == 0) {
        EXPECT_STREQ(serials[0].c_str(), serial0.c_str());
        EXPECT_STREQ(serials[1].c_str(), serial1.c_str());
        // Call RemoveMembership
        EXPECT_EQ(ER_OK, sapWithPeer2.RemoveMembership(serials[0], keyInfos[0]));

    } else {
        EXPECT_STREQ(serials[0].c_str(), serial1.c_str());
        EXPECT_STREQ(serials[1].c_str(), serial0.c_str());
        // Call RemoveMembership
        EXPECT_EQ(ER_OK, sapWithPeer2.RemoveMembership(serials[1], keyInfos[1]));
    }


    // Call GetProperty("MembershipSummaries"). This call should show 1 membership certificate
    EXPECT_EQ(ER_OK, sapWithPeer2.GetMembershipSummaries(arg)) << "GetMembershipSummaries failed.";
    count = arg.v_array.GetNumElements();
    EXPECT_EQ((uint32_t)1, count);
    delete [] serials;
    delete [] keyInfos;
    serials = new String[count];
    keyInfos = new KeyInfoNISTP256[count];
    EXPECT_EQ(ER_OK, sapWithPeer2.MsgArgToCertificateIds(arg, serials, keyInfos, count));
    EXPECT_EQ(count, (uint32_t)1);
    EXPECT_STREQ(serials[0].c_str(), "5678");
    delete [] serials;
    delete [] keyInfos;
}

TEST_F(SecurityManagementPolicyTest, remove_membership_fails_if_serial_does_not_match)
{
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);
    InstallMembershipOnManager();

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));

    KeyInfoNISTP256 peer2PublicKey;
    PermissionConfigurator& peer2PermissionConfigurator = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer2PermissionConfigurator.GetSigningPublicKey(peer2PublicKey));

    qcc::MembershipCertificate membershipCertificate[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("123",
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2PublicKey.GetPublicKey(),
                                                                    managerGuid,
                                                                    true,
                                                                    3600,
                                                                    membershipCertificate[0]
                                                                    ));

    // Call InstallMembership
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(membershipCertificate, 1)) << "Failed to install membership ";

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("456",
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2PublicKey.GetPublicKey(),
                                                                    managerGuid,
                                                                    true,
                                                                    3600,
                                                                    membershipCertificate[0]
                                                                    ));


    // Call InstallMembership
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(membershipCertificate, 1)) << "Failed to install membership ";

    // Call GetProperty("MembershipSummaries"). This call should show 2 membership certificates
    MsgArg arg;
    EXPECT_EQ(ER_OK, sapWithPeer2.GetMembershipSummaries(arg)) << "GetMembershipSummaries failed.";

    size_t count = arg.v_array.GetNumElements();
    EXPECT_EQ((uint32_t)2, count);
    String*serials = new String[count];
    KeyInfoNISTP256* keyInfos = new KeyInfoNISTP256[count];
    EXPECT_EQ(ER_OK, sapWithPeer2.MsgArgToCertificateIds(arg, serials, keyInfos, count));

    String serial0("123");
    String serial1("456");
    // Compare the serial  in the certificates just retrieved
    // Membership certs are stored as a non-deterministic set so the order can
    // change. We just want to make sure both certificates are returned. The
    // only time order will remain the same is if the certificates are in a
    // certificate chain.
    if (serials[0] == serial0) {
        EXPECT_STREQ(serials[0].c_str(), serial0.c_str());
        EXPECT_STREQ(serials[1].c_str(), serial1.c_str());
    } else {
        EXPECT_STREQ(serials[0].c_str(), serial1.c_str());
        EXPECT_STREQ(serials[1].c_str(), serial0.c_str());
    }

    // Call RemoveMembership
    String fakeSerial("333");
    EXPECT_EQ(ER_CERTIFICATE_NOT_FOUND, sapWithPeer2.RemoveMembership(fakeSerial, keyInfos[0]));
    delete [] serials;
    delete [] keyInfos;
}

TEST_F(SecurityManagementPolicyTest, remove_membership_fails_if_issuer_does_not_match)
{
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);
    InstallMembershipOnManager();
    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));

    KeyInfoNISTP256 peer2PublicKey;
    PermissionConfigurator& peer2PermissionConfigurator = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer2PermissionConfigurator.GetSigningPublicKey(peer2PublicKey));

    qcc::MembershipCertificate membershipCertificate[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("123",
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2PublicKey.GetPublicKey(),
                                                                    managerGuid,
                                                                    true,
                                                                    3600,
                                                                    membershipCertificate[0]
                                                                    ));

    // Call InstallMembership
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(membershipCertificate, 1)) << "Failed to install membership ";

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("456",
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2PublicKey.GetPublicKey(),
                                                                    managerGuid,
                                                                    true,
                                                                    3600,
                                                                    membershipCertificate[0]
                                                                    ));


    // Call InstallMembership
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(membershipCertificate, 1)) << "Failed to install membership ";

    // Call GetProperty("MembershipSummaries"). This call should show 2 membership certificates
    MsgArg arg;
    EXPECT_EQ(ER_OK, sapWithPeer2.GetMembershipSummaries(arg)) << "GetMembershipSummaries failed.";

    size_t count = arg.v_array.GetNumElements();
    EXPECT_EQ((uint32_t)2, count);
    String*serials = new String[count];
    KeyInfoNISTP256* keyInfos = new KeyInfoNISTP256[count];
    EXPECT_EQ(ER_OK, sapWithPeer2.MsgArgToCertificateIds(arg, serials, keyInfos, count));

    String serial0("123");
    String serial1("456");
    // Compare the serial  in the certificates just retrieved
    // Membership certs are stored as a non-deterministic set so the order can
    // change. We just want to make sure both certificates are returned. The
    // only time order will remain the same is if the certificates are in a
    // certificate chain.
    if (serials[0] == serial0) {
        EXPECT_STREQ(serials[0].c_str(), serial0.c_str());
        EXPECT_STREQ(serials[1].c_str(), serial1.c_str());
    } else {
        EXPECT_STREQ(serials[0].c_str(), serial1.c_str());
        EXPECT_STREQ(serials[1].c_str(), serial0.c_str());
    }

    // Call RemoveMembership
    EXPECT_EQ(ER_CERTIFICATE_NOT_FOUND, sapWithPeer2.RemoveMembership(serials[0], peer2PublicKey));
    delete [] serials;
    delete [] keyInfos;
}



TEST_F(SecurityManagementPolicyTest, successful_method_call_after_chained_membership_installation)
{
    BusAttachment busUsedAsCA("caBus");
    EXPECT_EQ(ER_OK, DeleteDefaultKeyStoreFile("caBus"));
    busUsedAsCA.Start();
    busUsedAsCA.Connect();

    DefaultECDHEAuthListener* caAuthListener;
    caAuthListener = new DefaultECDHEAuthListener();

    EXPECT_EQ(ER_OK, busUsedAsCA.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", caAuthListener));

    qcc::GUID128 guildAuthorityGUID;
    GUID128 leafGuid;
    GUID128 interGuid;
    GUID128 caGUID;

    PermissionMgmtTestHelper::GetGUID(peer1Bus, leafGuid);
    PermissionMgmtTestHelper::GetGUID(peer3Bus, interGuid);
    PermissionMgmtTestHelper::GetGUID(busUsedAsCA, caGUID);


    SecurityApplicationProxy sapWithManager(managerBus, managerBus.GetUniqueName().c_str(), managerToManagerSessionId);
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    uint8_t intermediateCN[] = { 9, 9, 9, 9 };
    uint8_t caCN[] = { 9, 9, 9, 9 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    //Create the CA cert
    qcc::IdentityCertificate caCert;
    caCert.SetSerial((uint8_t*)"1234", 5);
    caCert.SetIssuerCN(caCN, 4);
    caCert.SetSubjectCN(caCN, 4);
    CertificateX509::ValidPeriod validityCA;
    validityCA.validFrom = qcc::GetEpochTimestamp() / 1000;
    validityCA.validTo = validityCA.validFrom + TEN_MINS;
    caCert.SetValidity(&validityCA);

    KeyInfoNISTP256 caKey;
    PermissionConfigurator& caPC = busUsedAsCA.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, caPC.GetSigningPublicKey(caKey));

    caCert.SetSubjectPublicKey(caKey.GetPublicKey());
    caCert.SetAlias("ca-cert-alias");
    caCert.SetCA(true);

    //sign the ca cert
    caPC.SignCertificate(caCert);

    // Create the intermediate certificate using peer1
    qcc::IdentityCertificate peer1Cert;
    peer1Cert.SetSerial((uint8_t*)"2345", 5);
    peer1Cert.SetIssuerCN(caCN, 4);
    peer1Cert.SetSubjectCN(leafCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer1Cert.SetValidity(&validity);

    ECCPublicKey peer1PublicKey;
    GetAppPublicKey(peer1Bus, peer1PublicKey);

    peer1Cert.SetSubjectPublicKey(&peer1PublicKey);
    peer1Cert.SetAlias("intermediate-cert-alias");
    peer1Cert.SetCA(false);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, caPC.SignCertificate(peer1Cert));

    //We need identityCert chain CA1->Peer1
    const size_t certChainSize = 2;
    IdentityCertificate identityCertChain[certChainSize];
    identityCertChain[0] = peer1Cert;
    identityCertChain[1] = caCert;

    //
    // Create membership chain to be installed on peer 1
    //

    PermissionMgmtTestHelper::GetGUID(managerBus, guildAuthorityGUID);
    KeyInfoNISTP256 sgaKey;
    PermissionConfigurator& managerPC = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, managerPC.GetSigningPublicKey(sgaKey));

    // SGA membership certificate
    qcc::MembershipCertificate sgaMembershipCert;
    sgaMembershipCert.SetSerial((uint8_t*)"1234", 5);
    sgaMembershipCert.SetIssuerCN(managerCN, 4);
    sgaMembershipCert.SetSubjectCN(managerCN, 4);
    sgaMembershipCert.SetSubjectPublicKey(sgaKey.GetPublicKey());
    sgaMembershipCert.SetGuild(managerGuid);
    sgaMembershipCert.SetCA(true);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    sgaMembershipCert.SetValidity(&validity);

    managerPC.SignCertificate(sgaMembershipCert);

    MembershipCertificate managerMembershipCertChain[1];
    managerMembershipCertChain[0] = sgaMembershipCert;

    EXPECT_EQ(ER_OK, sapWithManager.InstallMembership(managerMembershipCertChain, 1));

    // Intermediate membership certificate
    qcc::MembershipCertificate intermediateMembershipCert;
    intermediateMembershipCert.SetSerial((uint8_t*)"2345", 5);
    intermediateMembershipCert.SetIssuerCN(managerCN, 4);
    intermediateMembershipCert.SetSubjectCN(intermediateCN, 4);
    KeyInfoNISTP256 interKey;
    PermissionConfigurator& peer3PC = peer3Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer3PC.GetSigningPublicKey(interKey));
    intermediateMembershipCert.SetSubjectPublicKey(interKey.GetPublicKey());
    intermediateMembershipCert.SetGuild(interGuid);
    intermediateMembershipCert.SetCA(true);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    intermediateMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    managerPC.SignCertificate(intermediateMembershipCert);

    // Leaf membership certificate
    qcc::MembershipCertificate leafMembershipCert;
    leafMembershipCert.SetSerial((uint8_t*)"3456", 5);
    leafMembershipCert.SetIssuerCN(intermediateCN, 4);
    leafMembershipCert.SetSubjectCN(leafCN, 4);
    KeyInfoNISTP256 leafKey;
    PermissionConfigurator& peer1PC = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PC.GetSigningPublicKey(leafKey));
    leafMembershipCert.SetSubjectPublicKey(leafKey.GetPublicKey());
    leafMembershipCert.SetGuild(leafGuid);
    leafMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    leafMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    peer3PC.SignCertificate(leafMembershipCert);

    const size_t membershipChainSize = 3;
    MembershipCertificate membershipCertChain[membershipChainSize];
    membershipCertChain[0] = leafMembershipCert;
    membershipCertChain[1] = intermediateMembershipCert;
    membershipCertChain[2] = sgaMembershipCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));
    EXPECT_EQ(ER_OK, peer3Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer3AuthListener));

    //EXPECT_EQ(ER_OK, sapWithPeer1.Reset());
    EXPECT_EQ(ER_OK, sapWithPeer1.InstallMembership(membershipCertChain, 3));

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(busUsedAsCA, identityCertChain[0], manifests[0]));
    // Call UpdateIdentity on Peer 1 to install the cert chain
    EXPECT_EQ(ER_OK, sapWithPeer1.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)))
        << "Failed to update Identity cert or manifest ";

    // Create the intermediate certificate using peer2
    qcc::IdentityCertificate peer2Cert;
    peer2Cert.SetSerial((uint8_t*)"5678", 5);
    peer2Cert.SetIssuerCN(caCN, 4);
    peer2Cert.SetSubjectCN(leafCN, 4);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer2Cert.SetValidity(&validity);

    KeyInfoNISTP256 peer2Key;
    PermissionConfigurator& peer2PC = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer2PC.GetSigningPublicKey(peer2Key));

    peer2Cert.SetSubjectPublicKey(peer2Key.GetPublicKey());
    peer2Cert.SetAlias("peer2-cert-alias");
    peer2Cert.SetCA(true);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, caPC.SignCertificate(peer2Cert));

    //We need identityCert chain CA1->Peer2
    identityCertChain[0] = peer2Cert;
    identityCertChain[1] = caCert;

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(busUsedAsCA, identityCertChain[0], manifests[0]));
    // Call UpdateIdentity on Peer 1 to install the cert chain
    EXPECT_EQ(ER_OK, sapWithPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)))
        << "Failed to update Identity cert or manifest ";

    uint32_t peer1ToPeer2SessionId;
    SessionOpts opts;
    EXPECT_EQ(ER_OK, peer1Bus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, peer1ToPeer2SessionId, opts));
    ProxyBusObject peer2Obj;
    peer2Obj = ProxyBusObject(peer1Bus, org::alljoyn::Bus::InterfaceName, org::alljoyn::Bus::ObjectPath, peer1ToPeer2SessionId, false);

    EXPECT_EQ(ER_OK, peer2Obj.IntrospectRemoteObject());
    delete caAuthListener;
}


TEST_F(SecurityManagementPolicyTest, unsuccessful_method_call_after_chained_membership_installation)
{
    BusAttachment busUsedAsCA("caBus");
    EXPECT_EQ(ER_OK, DeleteDefaultKeyStoreFile("caBus"));
    busUsedAsCA.Start();
    busUsedAsCA.Connect();

    DefaultECDHEAuthListener* caAuthListener;
    caAuthListener = new DefaultECDHEAuthListener();

    EXPECT_EQ(ER_OK, busUsedAsCA.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", caAuthListener));

    qcc::GUID128 guildAuthorityGUID;
    GUID128 leafGuid;
    GUID128 interGuid;
    GUID128 caGUID;

    PermissionMgmtTestHelper::GetGUID(peer1Bus, leafGuid);
    PermissionMgmtTestHelper::GetGUID(peer3Bus, interGuid);
    PermissionMgmtTestHelper::GetGUID(busUsedAsCA, caGUID);


    SecurityApplicationProxy sapWithManager(managerBus, managerBus.GetUniqueName().c_str(), managerToManagerSessionId);
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    uint8_t intermediateCN[] = { 9, 9, 9, 9 };
    uint8_t caCN[] = { 9, 9, 9, 9 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    //Create the CA cert
    qcc::IdentityCertificate caCert;
    caCert.SetSerial((uint8_t*)"1234", 5);
    caCert.SetIssuerCN(caCN, 4);
    caCert.SetSubjectCN(caCN, 4);
    CertificateX509::ValidPeriod validityCA;
    validityCA.validFrom = qcc::GetEpochTimestamp() / 1000;
    validityCA.validTo = validityCA.validFrom + TEN_MINS;
    caCert.SetValidity(&validityCA);

    KeyInfoNISTP256 caKey;
    PermissionConfigurator& caPC = busUsedAsCA.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, caPC.GetSigningPublicKey(caKey));

    caCert.SetSubjectPublicKey(caKey.GetPublicKey());
    caCert.SetAlias("ca-cert-alias");
    caCert.SetCA(true);

    //sign the ca cert
    caPC.SignCertificate(caCert);

    // Create the intermediate certificate using peer1
    qcc::IdentityCertificate peer1Cert;
    peer1Cert.SetSerial((uint8_t*)"2345", 5);
    peer1Cert.SetIssuerCN(caCN, 4);
    peer1Cert.SetSubjectCN(leafCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer1Cert.SetValidity(&validity);

    ECCPublicKey peer1PublicKey;
    sapWithPeer1.GetEccPublicKey(peer1PublicKey);

    peer1Cert.SetSubjectPublicKey(&peer1PublicKey);
    peer1Cert.SetAlias("intermediate-cert-alias");
    peer1Cert.SetCA(false);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, caPC.SignCertificate(peer1Cert));

    //We need identityCert chain CA1->Peer1
    const size_t certChainSize = 2;
    IdentityCertificate identityCertChain[certChainSize];
    identityCertChain[0] = peer1Cert;
    identityCertChain[1] = caCert;

    //
    // Create membership chain to be installed on peer 1
    //

    PermissionMgmtTestHelper::GetGUID(managerBus, guildAuthorityGUID);
    KeyInfoNISTP256 sgaKey;
    PermissionConfigurator& managerPC = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, managerPC.GetSigningPublicKey(sgaKey));

    // SGA membership certificate
    qcc::MembershipCertificate sgaMembershipCert;
    sgaMembershipCert.SetSerial((uint8_t*)"1234", 5);
    sgaMembershipCert.SetIssuerCN(managerCN, 4);
    sgaMembershipCert.SetSubjectCN(managerCN, 4);
    sgaMembershipCert.SetSubjectPublicKey(sgaKey.GetPublicKey());
    sgaMembershipCert.SetGuild(managerGuid);
    sgaMembershipCert.SetCA(true);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    sgaMembershipCert.SetValidity(&validity);

    managerPC.SignCertificate(sgaMembershipCert);

    MembershipCertificate managerMembershipCertChain[1];
    managerMembershipCertChain[0] = sgaMembershipCert;

    EXPECT_EQ(ER_OK, sapWithManager.InstallMembership(managerMembershipCertChain, 1));

    // Intermediate membership certificate
    qcc::MembershipCertificate intermediateMembershipCert;
    intermediateMembershipCert.SetSerial((uint8_t*)"2345", 5);
    intermediateMembershipCert.SetIssuerCN(managerCN, 4);
    intermediateMembershipCert.SetSubjectCN(intermediateCN, 4);
    KeyInfoNISTP256 interKey;
    PermissionConfigurator& peer3PC = peer3Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer3PC.GetSigningPublicKey(interKey));
    intermediateMembershipCert.SetSubjectPublicKey(interKey.GetPublicKey());
    intermediateMembershipCert.SetGuild(interGuid);
    intermediateMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    intermediateMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    managerPC.SignCertificate(intermediateMembershipCert);

    // Leaf membership certificate
    qcc::MembershipCertificate leafMembershipCert;
    leafMembershipCert.SetSerial((uint8_t*)"3456", 5);
    leafMembershipCert.SetIssuerCN(intermediateCN, 4);
    leafMembershipCert.SetSubjectCN(leafCN, 4);
    KeyInfoNISTP256 leafKey;
    PermissionConfigurator& peer1PC = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PC.GetSigningPublicKey(leafKey));
    leafMembershipCert.SetSubjectPublicKey(leafKey.GetPublicKey());
    leafMembershipCert.SetGuild(leafGuid);
    leafMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    leafMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    peer3PC.SignCertificate(leafMembershipCert);

    const size_t membershipChainSize = 3;
    MembershipCertificate membershipCertChain[membershipChainSize];
    membershipCertChain[0] = leafMembershipCert;
    membershipCertChain[1] = intermediateMembershipCert;
    membershipCertChain[2] = sgaMembershipCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));
    EXPECT_EQ(ER_OK, peer3Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer3AuthListener));

    EXPECT_EQ(ER_INVALID_CERTIFICATE, sapWithPeer1.InstallMembership(membershipCertChain, 3));
    delete caAuthListener;
}


TEST_F(SecurityManagementPolicyTest, chained_membership_signed_upto_ca_fails)
{
    BusAttachment busUsedAsCA("caBus");
    EXPECT_EQ(ER_OK, DeleteDefaultKeyStoreFile("caBus"));
    busUsedAsCA.Start();
    busUsedAsCA.Connect();

    DefaultECDHEAuthListener* caAuthListener;
    caAuthListener = new DefaultECDHEAuthListener();

    EXPECT_EQ(ER_OK, busUsedAsCA.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", caAuthListener));

    qcc::GUID128 guildAuthorityGUID;
    GUID128 leafGuid;
    GUID128 interGuid;
    GUID128 caGUID;

    PermissionMgmtTestHelper::GetGUID(peer1Bus, leafGuid);
    PermissionMgmtTestHelper::GetGUID(peer3Bus, interGuid);
    PermissionMgmtTestHelper::GetGUID(busUsedAsCA, caGUID);


    SecurityApplicationProxy sapWithManager(managerBus, managerBus.GetUniqueName().c_str(), managerToManagerSessionId);
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    uint8_t intermediateCN[] = { 9, 9, 9, 9 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    KeyInfoNISTP256 caKey;
    PermissionConfigurator& caPC = busUsedAsCA.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, caPC.GetSigningPublicKey(caKey));

    //
    // Create membership chain to be installed on peer 1
    //

    PermissionMgmtTestHelper::GetGUID(managerBus, guildAuthorityGUID);

    // SGA membership certificate
    qcc::MembershipCertificate caMembershipCert;
    caMembershipCert.SetSerial((uint8_t*)"1234", 5);
    caMembershipCert.SetIssuerCN(managerCN, 4);
    caMembershipCert.SetSubjectCN(managerCN, 4);
    caMembershipCert.SetSubjectPublicKey(caKey.GetPublicKey());
    caMembershipCert.SetGuild(managerGuid);
    caMembershipCert.SetCA(true);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    caMembershipCert.SetValidity(&validity);

    caPC.SignCertificate(caMembershipCert);

    InstallMembershipOnManager();

    // Intermediate membership certificate
    qcc::MembershipCertificate intermediateMembershipCert;
    intermediateMembershipCert.SetSerial((uint8_t*)"2345", 5);
    intermediateMembershipCert.SetIssuerCN(managerCN, 4);
    intermediateMembershipCert.SetSubjectCN(intermediateCN, 4);
    KeyInfoNISTP256 interKey;
    PermissionConfigurator& peer3PC = peer3Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer3PC.GetSigningPublicKey(interKey));
    intermediateMembershipCert.SetSubjectPublicKey(interKey.GetPublicKey());
    intermediateMembershipCert.SetGuild(interGuid);
    intermediateMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    intermediateMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    caPC.SignCertificate(intermediateMembershipCert);

    // Leaf membership certificate
    qcc::MembershipCertificate leafMembershipCert;
    leafMembershipCert.SetSerial((uint8_t*)"3456", 5);
    leafMembershipCert.SetIssuerCN(intermediateCN, 4);
    leafMembershipCert.SetSubjectCN(leafCN, 4);
    KeyInfoNISTP256 leafKey;
    PermissionConfigurator& peer1PC = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PC.GetSigningPublicKey(leafKey));
    leafMembershipCert.SetSubjectPublicKey(leafKey.GetPublicKey());
    leafMembershipCert.SetGuild(leafGuid);
    leafMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    leafMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    peer3PC.SignCertificate(leafMembershipCert);

    const size_t membershipChainSize = 3;
    MembershipCertificate membershipCertChain[membershipChainSize];
    membershipCertChain[0] = leafMembershipCert;
    membershipCertChain[1] = intermediateMembershipCert;
    membershipCertChain[2] = caMembershipCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));
    EXPECT_EQ(ER_OK, peer3Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer3AuthListener));

    EXPECT_EQ(ER_INVALID_CERTIFICATE, sapWithPeer1.InstallMembership(membershipCertChain, 3));
    delete caAuthListener;
}

TEST_F(SecurityManagementPolicyTest, chained_membership_with_two_levels_fails)
{
    BusAttachment busUsedAsCA("caBus");
    EXPECT_EQ(ER_OK, DeleteDefaultKeyStoreFile("caBus"));
    busUsedAsCA.Start();
    busUsedAsCA.Connect();

    DefaultECDHEAuthListener* caAuthListener;
    caAuthListener = new DefaultECDHEAuthListener();

    EXPECT_EQ(ER_OK, busUsedAsCA.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", caAuthListener));

    qcc::GUID128 guildAuthorityGUID;
    GUID128 leafGuid;
    GUID128 interGuid;
    GUID128 inter2Guid;
    GUID128 caGUID;

    PermissionMgmtTestHelper::GetGUID(peer1Bus, leafGuid);
    PermissionMgmtTestHelper::GetGUID(peer2Bus, inter2Guid);
    PermissionMgmtTestHelper::GetGUID(peer3Bus, interGuid);
    PermissionMgmtTestHelper::GetGUID(busUsedAsCA, caGUID);

    SecurityApplicationProxy sapWithManager(managerBus, managerBus.GetUniqueName().c_str(), managerToManagerSessionId);
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    uint8_t intermediateCN[] = { 9, 9, 9, 9 };
    uint8_t intermediate2CN[] = { 9, 9, 9, 9 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    KeyInfoNISTP256 caKey;
    PermissionConfigurator& caPC = busUsedAsCA.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, caPC.GetSigningPublicKey(caKey));

    //
    // Create membership chain to be installed on peer 1
    //

    PermissionMgmtTestHelper::GetGUID(managerBus, guildAuthorityGUID);

    // SGA membership certificate
    qcc::MembershipCertificate caMembershipCert;
    caMembershipCert.SetSerial((uint8_t*)"1234", 5);
    caMembershipCert.SetIssuerCN(managerCN, 4);
    caMembershipCert.SetSubjectCN(managerCN, 4);
    caMembershipCert.SetSubjectPublicKey(caKey.GetPublicKey());
    caMembershipCert.SetGuild(managerGuid);
    caMembershipCert.SetCA(true);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    caMembershipCert.SetValidity(&validity);

    caPC.SignCertificate(caMembershipCert);

    InstallMembershipOnManager();

    // Intermediate membership certificate
    qcc::MembershipCertificate intermediateMembershipCert;
    intermediateMembershipCert.SetSerial((uint8_t*)"2345", 5);
    intermediateMembershipCert.SetIssuerCN(managerCN, 4);
    intermediateMembershipCert.SetSubjectCN(intermediateCN, 4);
    KeyInfoNISTP256 interKey;
    PermissionConfigurator& peer3PC = peer3Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer3PC.GetSigningPublicKey(interKey));
    intermediateMembershipCert.SetSubjectPublicKey(interKey.GetPublicKey());
    intermediateMembershipCert.SetGuild(interGuid);
    intermediateMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    intermediateMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    caPC.SignCertificate(intermediateMembershipCert);

    // Intermediate 2 membership certificate
    qcc::MembershipCertificate intermediate2MembershipCert;
    intermediate2MembershipCert.SetSerial((uint8_t*)"2345", 5);
    intermediate2MembershipCert.SetIssuerCN(intermediateCN, 4);
    intermediate2MembershipCert.SetSubjectCN(intermediate2CN, 4);
    KeyInfoNISTP256 inter2Key;
    PermissionConfigurator& peer2PC = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer2PC.GetSigningPublicKey(inter2Key));
    intermediate2MembershipCert.SetSubjectPublicKey(inter2Key.GetPublicKey());
    intermediate2MembershipCert.SetGuild(interGuid);
    intermediate2MembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    intermediate2MembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    peer3PC.SignCertificate(intermediateMembershipCert);

    // Leaf membership certificate
    qcc::MembershipCertificate leafMembershipCert;
    leafMembershipCert.SetSerial((uint8_t*)"3456", 5);
    leafMembershipCert.SetIssuerCN(intermediate2CN, 4);
    leafMembershipCert.SetSubjectCN(leafCN, 4);
    KeyInfoNISTP256 leafKey;
    PermissionConfigurator& peer1PC = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PC.GetSigningPublicKey(leafKey));
    leafMembershipCert.SetSubjectPublicKey(leafKey.GetPublicKey());
    leafMembershipCert.SetGuild(leafGuid);
    leafMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    leafMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    peer2PC.SignCertificate(leafMembershipCert);

    const size_t membershipChainSize = 4;
    MembershipCertificate membershipCertChain[membershipChainSize];
    membershipCertChain[0] = leafMembershipCert;
    membershipCertChain[1] = intermediate2MembershipCert;
    membershipCertChain[2] = intermediateMembershipCert;
    membershipCertChain[3] = caMembershipCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));
    EXPECT_EQ(ER_OK, peer3Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer3AuthListener));

    EXPECT_EQ(ER_FAIL, sapWithPeer1.InstallMembership(membershipCertChain, 4));
    delete caAuthListener;
}

TEST_F(SecurityManagementPolicyTest, unsuccessful_method_call_when_sga_delegation_is_false)
{
    BusAttachment busUsedAsCA("caBus");
    EXPECT_EQ(ER_OK, DeleteDefaultKeyStoreFile("caBus"));
    busUsedAsCA.Start();
    busUsedAsCA.Connect();

    DefaultECDHEAuthListener* caAuthListener;
    caAuthListener = new DefaultECDHEAuthListener();

    EXPECT_EQ(ER_OK, busUsedAsCA.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", caAuthListener));

    qcc::GUID128 guildAuthorityGUID;
    GUID128 leafGuid;
    GUID128 interGuid;
    GUID128 caGUID;

    PermissionMgmtTestHelper::GetGUID(peer1Bus, leafGuid);
    PermissionMgmtTestHelper::GetGUID(peer3Bus, interGuid);
    PermissionMgmtTestHelper::GetGUID(busUsedAsCA, caGUID);


    SecurityApplicationProxy sapWithManager(managerBus, managerBus.GetUniqueName().c_str(), managerToManagerSessionId);
    SecurityApplicationProxy sapWithPeer1(managerBus, peer1Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    uint8_t managerCN[] = { 1, 2, 3, 4 };
    //uint8_t intermediateCN[] = { 9, 9, 9, 9 };
    uint8_t caCN[] = { 9, 9, 9, 9 };
    uint8_t leafCN[] = { 9, 0, 1, 2 };

    //Create the CA cert
    qcc::IdentityCertificate caCert;
    caCert.SetSerial((uint8_t*)"1234", 5);
    caCert.SetIssuerCN(caCN, 4);
    caCert.SetSubjectCN(caCN, 4);
    CertificateX509::ValidPeriod validityCA;
    validityCA.validFrom = qcc::GetEpochTimestamp() / 1000;
    validityCA.validTo = validityCA.validFrom + TEN_MINS;
    caCert.SetValidity(&validityCA);

    KeyInfoNISTP256 caKey;
    PermissionConfigurator& caPC = busUsedAsCA.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, caPC.GetSigningPublicKey(caKey));

    caCert.SetSubjectPublicKey(caKey.GetPublicKey());
    caCert.SetAlias("ca-cert-alias");
    caCert.SetCA(true);

    //sign the ca cert
    caPC.SignCertificate(caCert);

    // Create the intermediate certificate using peer1
    qcc::IdentityCertificate peer1Cert;
    peer1Cert.SetSerial((uint8_t*)"2345", 5);
    peer1Cert.SetIssuerCN(caCN, 4);
    peer1Cert.SetSubjectCN(leafCN, 4);
    CertificateX509::ValidPeriod validity;
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    peer1Cert.SetValidity(&validity);

    ECCPublicKey peer1PublicKey;
    sapWithPeer1.GetEccPublicKey(peer1PublicKey);

    peer1Cert.SetSubjectPublicKey(&peer1PublicKey);
    peer1Cert.SetAlias("intermediate-cert-alias");
    peer1Cert.SetCA(false);

    //sign the leaf cert
    EXPECT_EQ(ER_OK, caPC.SignCertificate(peer1Cert));

    //We need identityCert chain CA1->Peer1
    const size_t certChainSize = 2;
    IdentityCertificate identityCertChain[certChainSize];
    identityCertChain[0] = peer1Cert;
    identityCertChain[1] = caCert;

    //
    // Create membership chain to be installed on peer 1
    //

    PermissionMgmtTestHelper::GetGUID(managerBus, guildAuthorityGUID);
    KeyInfoNISTP256 sgaKey;
    PermissionConfigurator& managerPC = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, managerPC.GetSigningPublicKey(sgaKey));

    // SGA membership certificate
    qcc::MembershipCertificate sgaMembershipCert;
    sgaMembershipCert.SetSerial((uint8_t*)"1234", 5);
    sgaMembershipCert.SetIssuerCN(managerCN, 4);
    sgaMembershipCert.SetSubjectCN(managerCN, 4);
    sgaMembershipCert.SetSubjectPublicKey(sgaKey.GetPublicKey());
    sgaMembershipCert.SetGuild(managerGuid);
    sgaMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    sgaMembershipCert.SetValidity(&validity);

    managerPC.SignCertificate(sgaMembershipCert);

    MembershipCertificate managerMembershipCertChain[1];
    managerMembershipCertChain[0] = sgaMembershipCert;

    EXPECT_EQ(ER_OK, sapWithManager.InstallMembership(managerMembershipCertChain, 1));

    // Leaf membership certificate
    qcc::MembershipCertificate leafMembershipCert;
    leafMembershipCert.SetSerial((uint8_t*)"3456", 5);
    leafMembershipCert.SetIssuerCN(managerCN, 4);
    leafMembershipCert.SetSubjectCN(leafCN, 4);
    KeyInfoNISTP256 leafKey;
    PermissionConfigurator& peer1PC = peer1Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, peer1PC.GetSigningPublicKey(leafKey));
    leafMembershipCert.SetSubjectPublicKey(leafKey.GetPublicKey());
    leafMembershipCert.SetGuild(leafGuid);
    leafMembershipCert.SetCA(false);
    validity.validFrom = qcc::GetEpochTimestamp() / 1000;
    validity.validTo = validity.validFrom + TEN_MINS;
    leafMembershipCert.SetValidity(&validity);
    /* use the signing bus to sign the cert */
    managerPC.SignCertificate(leafMembershipCert);

    const size_t membershipChainSize = 2;
    MembershipCertificate membershipCertChain[membershipChainSize];
    membershipCertChain[0] = leafMembershipCert;
    membershipCertChain[1] = sgaMembershipCert;

    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener));
    EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener));
    EXPECT_EQ(ER_OK, peer3Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer3AuthListener));

    //EXPECT_EQ(ER_OK, sapWithPeer1.Reset());
    EXPECT_EQ(ER_INVALID_CERTIFICATE, sapWithPeer1.InstallMembership(membershipCertChain, 2));
    delete caAuthListener;
}

/*
 * Purpose:
 * ASG members  can also call methods from the org.alljoyn.Bus.Security.ManagedApplication
 * interface on the app. bus in the default policy.
 *
 * Setup:
 * ASGA bus claims the app. bus.
 * app. bus has default policy installed.
 *
 * ASG bus calls UpdateIdentity on the app. bus.
 * ASG bus calls GetIdentity on the app. bus.
 *
 * ASG bus calls UpdatePolicy on the app. bus
 * ASG bus calls GetPolicy on the app. bus
 *
 * ASG bus calls ResetPolicy on the app. bus
 * ASG bus calls GetPolicy on the app. bus
 *
 * ASG bus calls InstallMembership on the app. bus
 * ASG bus calls GetMembershipSummaries on the app. bus
 *
 * ASG bus calls StartManagement on the app. bus
 * ASG bus calls EndManagement on the app. bus
 * ASG bus calls RemoveMembership on the app. bus
 *
 * ASG bus calls Reset on the app. bus
 *
 * Verification:
 * GetPolicy should fetch the policy with no rules.
 *
 * UpdaterIdentity should be successful.
 * GetProperty("Identity") should return the same identity certificate as before.
 *
 * UpdatePolicy should be successful.
 * GetProperty("Policy") should return the same policy as before.
 *
 * ResetPolicy should be successful.
 * GetProperty("Policy") should return the default policy.
 *
 * InstallMembership should be successful.
 * GetProperty("MembershipSummaries") should return the details about the membership certificates installed.
 *
 * RemoveMembership should be sucessful.
 *
 * Reset should be successful.
 *
 * StartManagement and EndManagement callbacks should arrive when appropriate.
 *
 * Peer1 = ASG bus
 * Peer2 = app. bus
 */
TEST_F(SecurityManagementPolicyTest, admin_security_group_members_can_also_call_members_for_managedapplication_default_policy)
{
    InstallMembershipOnManager();
    InstallMembershipOnPeer1();
    InstallMembershipOnPeer2();

    SessionOpts opts;
    uint32_t sessionId;
    EXPECT_EQ(ER_OK, peer1Bus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, sessionId, opts));

    SecurityApplicationProxy sapWithPeer1toPeer2(peer1Bus, peer2Bus.GetUniqueName().c_str());

    // Call UpdateIdentity
    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));


    // Get manager key
    KeyInfoNISTP256 peer2Key;
    PermissionConfigurator& pcPeer2 = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, pcPeer2.GetSigningPublicKey(peer2Key));

    // Create identityCert
    const size_t certChainSize = 1;
    IdentityCertificate identityCertChain[certChainSize];
    GUID128 guid;


    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateIdentityCert(managerBus,
                                                                  "1",
                                                                  managerGuid.ToString(),
                                                                  peer2Key.GetPublicKey(),
                                                                  "Alias",
                                                                  3600,
                                                                  identityCertChain[0])) << "Failed to create identity certificate.";
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(managerBus, identityCertChain[0], manifests[0]));
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)));
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.SecureConnection(true));

    MsgArg identityArg;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetIdentity(identityArg));

    IdentityCertificate identityCertChain_out[certChainSize];
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.MsgArgToIdentityCertChain(identityArg, identityCertChain_out, 1));

    ASSERT_EQ(identityCertChain[0].GetSerialLen(), identityCertChain_out[0].GetSerialLen());
    for (size_t i = 0; i < identityCertChain[0].GetSerialLen(); ++i) {
        EXPECT_EQ(identityCertChain[0].GetSerial()[i], identityCertChain_out[0].GetSerial()[i]);
    }

    PermissionPolicy policy;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetPolicy(policy));

    // Assume the default policy which is always 0
    EXPECT_EQ(static_cast<uint32_t>(0), policy.GetVersion());

    policy.SetVersion(policy.GetVersion() + 1);
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.UpdatePolicy(policy));
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.SecureConnection(true));

    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetPolicy(policy));

    EXPECT_EQ(static_cast<uint32_t>(1), policy.GetVersion());

    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.ResetPolicy());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetPolicy(policy));

    // Reset back to the default policy which is always 0
    EXPECT_EQ(static_cast<uint32_t>(0), policy.GetVersion());

    String membershipSerial = "2";
    qcc::MembershipCertificate peer2MembershipCertificate[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert(membershipSerial,
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2Key.GetPublicKey(),
                                                                    managerGuid,
                                                                    false,
                                                                    3600,
                                                                    peer2MembershipCertificate[0]
                                                                    ));
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.InstallMembership(peer2MembershipCertificate, 1));

    MsgArg membershipSummariesArg;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetMembershipSummaries(membershipSummariesArg));

    // Call GetProperty("MembershipSummaries"). This call should show 2 membership certificates
    size_t count = membershipSummariesArg.v_array.GetNumElements();
    EXPECT_EQ((uint32_t)2, count);
    String*serials = new String[count];
    KeyInfoNISTP256* keyInfos = new KeyInfoNISTP256[count];
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.MsgArgToCertificateIds(membershipSummariesArg, serials, keyInfos, count));

    String serial0("2");
    String serial1("1");
    // Compare the serial  in the certificates just retrieved
    // Membership certs are stored as a non-deterministic set so the order can
    // change. We just want to make sure both certificates are returned. The
    // only time order will remain the same is if the certificates are in a
    // certificate chain.
    if (serials[0] == serial0) {
        EXPECT_STREQ(serials[0].c_str(), serial0.c_str());
        EXPECT_STREQ(serials[1].c_str(), serial1.c_str());
    } else {
        EXPECT_STREQ(serials[0].c_str(), serial1.c_str());
        EXPECT_STREQ(serials[1].c_str(), serial0.c_str());
    }

    // Get manager key
    KeyInfoNISTP256 managerKey;
    PermissionConfigurator& pcManager = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, pcManager.GetSigningPublicKey(managerKey));

    // StartManagement
    PermissionConfigurator::ApplicationState applicationState;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.StartManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_TRUE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // StartManagement again
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_MANAGEMENT_ALREADY_STARTED, sapWithPeer1toPeer2.StartManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // EndManagement
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.EndManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_TRUE(peer2ConfigurationListener.endManagementReceived);

    // EndManagement again
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_MANAGEMENT_NOT_STARTED, sapWithPeer1toPeer2.EndManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // StartManagement again
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.StartManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_TRUE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // RemoveMembership should succeed
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.RemoveMembership("2", managerKey));

    // Reset should succeed
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.Reset());
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // EndManagement fails because the target app is now in CLAIMABLE state
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.EndManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMABLE, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);
    delete [] serials;
    delete [] keyInfos;
}

/*
   Purpose:
   ASG members call GetAllProperties on the org.alljoyn.Bus.Security.ManagedApplication Interface on the app.
   Bus  in the default policy.

   Setup:
   ASGA bus claims the app. bus.
   app. bus has default policy installed.

   App. bus also has 3 membership certiificates installed.
   The identity certificate of the app. bus should be a certificate chain with 3 certificates i.e CA->Intermediate CA->leaf

   ASG bus calls GetAllProperties on the app. bus.

   Verfication:
   ASG bus should fetch the following properties successfully:

   Version
   Identity
   Manifest
   IdentityCertificateId
   PolicyVersion
   Policy
   DefaultPolicy
   MembershipSummaries

   Peer1 = ASG bus
   Peer2 = app. bus

 */

TEST_F(SecurityManagementPolicyTest, admin_security_group_members_call_getallproperties_for_managedapplication_default_policy)
{
    InstallMembershipOnManager();
    InstallMembershipOnPeer1();
    InstallMembershipOnPeer2();

    // Install 2 more membership certificates on the app bus : Peer2
    // 1 is installed already installed in the call to InstallMembershipOnPeer2()
    // Create peer2 key
    KeyInfoNISTP256 peer2Key;
    PermissionConfigurator& pcPeer2 = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, pcPeer2.GetSigningPublicKey(peer2Key));

    const qcc::GUID128 mem2Guid;
    String membershipSerial = "2";
    qcc::MembershipCertificate peer2MembershipCertificate[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert(membershipSerial,
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2Key.GetPublicKey(),
                                                                    mem2Guid,
                                                                    false,
                                                                    3600,
                                                                    peer2MembershipCertificate[0]
                                                                    ));
    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(peer2MembershipCertificate, 1));

    const qcc::GUID128 mem3Guid;
    membershipSerial = "3";
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert(membershipSerial,
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2Key.GetPublicKey(),
                                                                    mem3Guid,
                                                                    false,
                                                                    3600,
                                                                    peer2MembershipCertificate[0]
                                                                    ));
    EXPECT_EQ(ER_OK, sapWithPeer2.InstallMembership(peer2MembershipCertificate, 1));

    SessionOpts opts;
    uint32_t sessionId;
    EXPECT_EQ(ER_OK, peer1Bus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, sessionId, opts));

    SecurityApplicationProxy sapWithPeer1toPeer2(peer1Bus, peer2Bus.GetUniqueName().c_str());

    // Call UpdateIdentity
    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    // Create identityCert
    const size_t certChainSize = 3;
    IdentityCertificate identityCertChain[certChainSize];
    GUID128 guid;

    qcc::GUID128 peer2Guid(0);
    PermissionMgmtTestHelper::GetGUID(peer2Bus, peer2Guid);

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateIdentityCertChain(managerBus,
                                                                       managerBus,
                                                                       "2",
                                                                       peer2Guid.ToString(),
                                                                       peer2Key.GetPublicKey(),
                                                                       "Alias",
                                                                       3600,
                                                                       identityCertChain,
                                                                       certChainSize));

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(managerBus, identityCertChain[0], manifests[0]));
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)));
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.SecureConnection(true));

    // Call GetAllProperties

    MsgArg props;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetAllProperties(org::alljoyn::Bus::Security::ManagedApplication::InterfaceName, props));

    MsgArg* propArg;
    EXPECT_EQ(ER_OK, props.GetElement("{sv}", "Version", &propArg));
    EXPECT_EQ(ER_OK, props.GetElement("{sv}", "Identity", &propArg));
    EXPECT_EQ(ER_OK, props.GetElement("{sv}", "Manifests", &propArg));
    EXPECT_EQ(ER_OK, props.GetElement("{sv}", "IdentityCertificateId", &propArg));
    EXPECT_EQ(ER_OK, props.GetElement("{sv}", "PolicyVersion", &propArg));
    EXPECT_EQ(ER_OK, props.GetElement("{sv}", "Policy", &propArg));
    EXPECT_EQ(ER_OK, props.GetElement("{sv}", "DefaultPolicy", &propArg));
    EXPECT_EQ(ER_OK, props.GetElement("{sv}", "MembershipSummaries", &propArg));
}



/*
 * Purpose:
 * Non ASG members should not be able to access org.alljoyn.Bus.Security.ManagedApplication
 * interface on the app. bus in the default policy.
 *
 * Setup:
 * ASGA bus claims the app. bus.
 * app. bus has default policy installed.
 *
 * non ASG bus calls the following methods on the app. bus:
 * Reset
 * UpdateIdentity
 * UpdatePolicy
 * ResetPolicy
 * InstallMembership
 * RemoveMembership
 * StartManagement
 * EndManagement
 *
 * non ASG bus tries to fetch the following properties on the  app. bus:
 * Version
 * Identity
 * Manifest
 * IdentityCertificateId
 * PolicyVersion
 * Policy
 * DefaultPolicy
 * MembershipSummaries
 *
 * non ASG bus calls GetAllProperties on the org.alljoyn.Bus.Security.ManagedApplication
 * interface on the app. bus.
 *
 * Verification:
 * All methods will fail as Non ASG members cannot call these methods on the app. bus.
 *
 * The non ASG bus should not be able to fetch  any property on the app. bus.
 * All the GetProperty calls should fail with permission denied.
 *
 * All properties can be called by ASG member only. This is because the IRB spec
 * says: The ManagedApplication is an interface that provides the mechanism for
 * an admin to manage the application's security configuration.
 *
 * Peer1 = ASG bus
 * Peer2 = app. bus
 */
TEST_F(SecurityManagementPolicyTest, non_group_members_can_not_call_managedapplication)
{
    InstallMembershipOnManager();
    InstallMembershipOnPeer2();

    SessionOpts opts;
    uint32_t sessionId;
    EXPECT_EQ(ER_OK, peer1Bus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, sessionId, opts));

    SecurityApplicationProxy sapWithPeer1toPeer2(peer1Bus, peer2Bus.GetUniqueName().c_str());

    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.Reset());

    // StartManagement and EndManagement should fail before setting up policies
    PermissionConfigurator::ApplicationState applicationState;
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);

    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.StartManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.EndManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // Call UpdateIdentity
    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    // Get manager key
    KeyInfoNISTP256 peer2Key;
    PermissionConfigurator& pcPeer2 = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, pcPeer2.GetSigningPublicKey(peer2Key));

    // Create identityCert
    const size_t certChainSize = 1;
    IdentityCertificate identityCertChain[certChainSize];
    GUID128 guid;

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateIdentityCert(managerBus,
                                                                  "1",
                                                                  managerGuid.ToString(),
                                                                  peer2Key.GetPublicKey(),
                                                                  "Alias",
                                                                  3600,
                                                                  identityCertChain[0])) << "Failed to create identity certificate.";

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(managerBus, identityCertChain[0], manifests[0]));
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)));


    PermissionPolicy policy;
    CreatePermissivePolicy(policy, 1);
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.UpdatePolicy(policy));

    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.ResetPolicy());

    String membershipSerial = "2";
    qcc::MembershipCertificate peer2MembershipCertificate[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert(membershipSerial,
                                                                    managerBus,
                                                                    peer2Bus.GetUniqueName(),
                                                                    peer2Key.GetPublicKey(),
                                                                    managerGuid,
                                                                    false,
                                                                    3600,
                                                                    peer2MembershipCertificate[0]
                                                                    ));
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.InstallMembership(peer2MembershipCertificate, 1));

    // Get manager key
    KeyInfoNISTP256 managerKey;
    PermissionConfigurator& pcManager = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, pcManager.GetSigningPublicKey(managerKey));

    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.RemoveMembership("1", managerKey));

    MsgArg identityCertArg;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.GetIdentity(identityCertArg));
    std::vector<Manifest> retrievedManifests;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.GetManifests(retrievedManifests));
    String serial;
    qcc::KeyInfoNISTP256 issuerKey;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.GetIdentityCertificateId(serial, issuerKey));
    uint32_t policyVersion;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.GetPolicyVersion(policyVersion));
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.GetPolicy(policy));
    MsgArg membershipSummariesArg;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.GetMembershipSummaries(membershipSummariesArg));

    // StartManagement and EndManagement should fail, since the policy doesn't allow them
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);

    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.StartManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.EndManagement());
    EXPECT_EQ(ER_OK, sapWithPeer1toPeer2.GetApplicationState(applicationState));
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);
}

/*
 * Test is identical to non_group_members_can_not_call_managedapplication except
 * it only tests the GetManagedApplicationVersion call. This way the rest of the
 * code can be tested till ASACORE-2557 is fixed
 */
// Please re-enable test once ASACORE-2557 is fixed.
TEST_F(SecurityManagementPolicyTest, DISABLED_non_group_members_can_not_get_managedapplication_version)
{
    InstallMembershipOnManager();
    InstallMembershipOnPeer2();

    SessionOpts opts;
    uint32_t sessionId;
    EXPECT_EQ(ER_OK, peer1Bus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, sessionId, opts));

    SecurityApplicationProxy sapWithPeer1toPeer2(peer1Bus, peer2Bus.GetUniqueName().c_str());

    uint16_t managedAppVersion;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapWithPeer1toPeer2.GetManagedApplicationVersion(managedAppVersion));
}

/*
 * Purpose:
 * If an app. bus has a policy that allows all inbound messages, anybody can
 * manage the device.
 *
 * Setup:
 * ASGA bus claims the app. bus.
 *
 * App. bus has the following policy installed:
 * Peer type: ALL
 * Rule: Object path: *, Interface Name: *; Action mask: PROVIDE|MODIFY|OBSERVE
 * A ECDHE_NULL session is set between the app. bus and the non-ASG bus.
 * The non ASG bus calls Reset on the app. bus
 *
 * Verification:
 * StartManagement, EndManagement and Reset should be successful.
 */
TEST_F(SecurityManagementPolicyTest, non_members_can_call_managedapplication_methods_if_policy_allows)
{
    InstallMembershipOnManager();
    InstallMembershipOnPeer2();

    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer1SessionId);
    PermissionPolicy defaultPolicy;
    EXPECT_EQ(ER_OK, sapWithPeer2.GetDefaultPolicy(defaultPolicy));
    PermissionPolicy policy;
    CreatePermissivePolicy(policy, 1);
    EXPECT_EQ(ER_OK, UpdatePolicyWithValuesFromDefaultPolicy(defaultPolicy, policy, true, true, true));

    EXPECT_EQ(ER_OK, sapWithPeer2.UpdatePolicy(policy));
    EXPECT_EQ(ER_OK, sapWithPeer2.SecureConnection(true));

    BusAttachment nonASGBus("non-ASGBus", true);
    EXPECT_EQ(ER_OK, nonASGBus.Start());
    EXPECT_EQ(ER_OK, nonASGBus.Connect());

    InMemoryKeyStoreListener keyStoreListener;
    EXPECT_EQ(ER_OK, nonASGBus.RegisterKeyStoreListener(keyStoreListener));

    DefaultECDHEAuthListener authListener;
    EXPECT_EQ(ER_OK, nonASGBus.EnablePeerSecurity("ALLJOYN_ECDHE_NULL", &authListener, nullptr, false, &managerConfigurationListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA ALLJOYN_ECDHE_NULL", peer2AuthListener, nullptr, false, &peer2ConfigurationListener));

    SessionOpts opts;
    uint32_t sessionId;
    EXPECT_EQ(ER_OK, nonASGBus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, sessionId, opts));

    SecurityApplicationProxy sapWithNonASGBustoPeer2(nonASGBus, peer2Bus.GetUniqueName().c_str());

    // Policy updated must secure connection to update keys.
    EXPECT_EQ(ER_OK, sapWithNonASGBustoPeer2.SecureConnection(true));

    // StartManagement and EndManagement should succeed
    PermissionConfigurator::ApplicationState applicationState;
    EXPECT_EQ(ER_OK, sapWithNonASGBustoPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);

    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapWithNonASGBustoPeer2.StartManagement());
    EXPECT_EQ(ER_OK, sapWithNonASGBustoPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_TRUE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapWithNonASGBustoPeer2.EndManagement());
    EXPECT_EQ(ER_OK, sapWithNonASGBustoPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_TRUE(peer2ConfigurationListener.endManagementReceived);

    // Reset should succeed too
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapWithNonASGBustoPeer2.Reset());
    EXPECT_EQ(ER_OK, sapWithNonASGBustoPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMABLE, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    EXPECT_EQ(ER_OK, nonASGBus.Stop());
    EXPECT_EQ(ER_OK, nonASGBus.Join());
}

TEST_F(SecurityManagementPolicyTest, end_management_after_reset)
{
    InstallMembershipOnManager();

    // Create management session to peer2
    SessionOpts opts;
    uint32_t sessionId;
    EXPECT_EQ(ER_OK, managerBus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, sessionId, opts));
    SecurityApplicationProxy sapToPeer2(managerBus, peer2Bus.GetUniqueName().c_str());

    // StartManagement
    PermissionConfigurator::ApplicationState applicationState;
    EXPECT_EQ(ER_OK, sapToPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapToPeer2.StartManagement());
    EXPECT_EQ(ER_OK, sapToPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_TRUE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // Reset
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapToPeer2.Reset());
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // EndManagement fails because the target app is now in CLAIMABLE state
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_PERMISSION_DENIED, sapToPeer2.EndManagement());
    EXPECT_EQ(ER_OK, sapToPeer2.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMABLE, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // Claim the target app again
    delete managerAuthListener;
    delete peer2AuthListener;
    managerAuthListener = new DefaultECDHEAuthListener();
    peer2AuthListener = new DefaultECDHEAuthListener();
    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", managerAuthListener, nullptr, false, &managerConfigurationListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_NULL ALLJOYN_ECDHE_ECDSA", peer2AuthListener, nullptr, false, &peer2ConfigurationListener));

    SetManifestTemplate(peer2Bus);

    Manifest manifests[1];
    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));

    // Get manager key
    KeyInfoNISTP256 managerKey;
    PermissionConfigurator& pcManager = managerBus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, pcManager.GetSigningPublicKey(managerKey));

    // Create peer2 key
    KeyInfoNISTP256 peer2Key;
    PermissionConfigurator& pcPeer2 = peer2Bus.GetPermissionConfigurator();
    EXPECT_EQ(ER_OK, pcPeer2.GetSigningPublicKey(peer2Key));

    // Create peer2 identityCert
    const size_t certChainSize = 1;
    IdentityCertificate identityCertChainPeer2[certChainSize];

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateIdentityCert(managerBus,
                                                                  "0",
                                                                  managerGuid.ToString(),
                                                                  peer2Key.GetPublicKey(),
                                                                  "Peer2Alias",
                                                                  3600,
                                                                  identityCertChainPeer2[0])) << "Failed to create identity certificate.";

    // Claim peer2
    SessionOpts opts2;
    EXPECT_EQ(ER_OK, managerBus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, managerToPeer2SessionId, opts2));

    SecurityApplicationProxy sapWithPeer2(managerBus, peer2Bus.GetUniqueName().c_str(), managerToPeer2SessionId);
    PermissionConfigurator::ApplicationState applicationStatePeer2;
    EXPECT_EQ(ER_OK, sapWithPeer2.GetApplicationState(applicationStatePeer2));
    EXPECT_EQ(PermissionConfigurator::CLAIMABLE, applicationStatePeer2);

    EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(managerBus, identityCertChainPeer2[0], manifests[0]));
    EXPECT_EQ(ER_OK, sapWithPeer2.Claim(managerKey,
                                        managerGuid,
                                        managerKey,
                                        identityCertChainPeer2, certChainSize,
                                        manifests, ArraySize(manifests)));

    for (uint32_t msec = 0; msec < LOOP_END_10000; msec += WAIT_TIME_5) {
        if (appStateListener.isClaimed(peer2Bus.GetUniqueName())) {
            break;
        }
        qcc::Sleep(WAIT_TIME_5);
    }

    ASSERT_EQ(PermissionConfigurator::ApplicationState::CLAIMED, appStateListener.stateMap[peer2Bus.GetUniqueName()]);

    // Switch to ECDHE_ECDSA-only
    EXPECT_EQ(ER_OK, managerBus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", managerAuthListener, nullptr, false, &managerConfigurationListener));
    EXPECT_EQ(ER_OK, peer1Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer1AuthListener, nullptr, false, &peer1ConfigurationListener));
    EXPECT_EQ(ER_OK, peer2Bus.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", peer2AuthListener, nullptr, false, &peer2ConfigurationListener));

    // Create post-Reset & Claim management session
    EXPECT_EQ(ER_OK, managerBus.JoinSession(peer2Bus.GetUniqueName().c_str(), peer2SessionPort, NULL, sessionId, opts));
    SecurityApplicationProxy sapForEndManagement(managerBus, peer2Bus.GetUniqueName().c_str());

    // StartManagement returns already-started
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_MANAGEMENT_ALREADY_STARTED, sapForEndManagement.StartManagement());
    EXPECT_EQ(ER_OK, sapForEndManagement.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);

    // EndManagement succeeds
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_OK, sapForEndManagement.EndManagement());
    EXPECT_EQ(ER_OK, sapForEndManagement.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_TRUE(peer2ConfigurationListener.endManagementReceived);

    // EndManagement returns not-started
    peer2ConfigurationListener.startManagementReceived = false;
    peer2ConfigurationListener.endManagementReceived = false;
    EXPECT_EQ(ER_MANAGEMENT_NOT_STARTED, sapForEndManagement.EndManagement());
    EXPECT_EQ(ER_OK, sapForEndManagement.GetApplicationState(applicationState));
    EXPECT_EQ(PermissionConfigurator::CLAIMED, applicationState);
    EXPECT_FALSE(peer2ConfigurationListener.startManagementReceived);
    EXPECT_FALSE(peer2ConfigurationListener.endManagementReceived);
}

/*
 * Purpose:
 * Before claim, any peer trying to call methods on the
 * org.alljoyn.Bus.Security.ManagedApplication interface should fail.
 *
 * Setup:
 * Establish an ECDHE_ECDSA based session between peer1 and peer2.
 *
 * Peer1 calls the following methods on the peer2:
 *
 * Reset
 * UpdateIdentity
 * UpdatePolicy
 * ResetPolicy
 * InstallMembership
 * RemoveMembership
 *
 * Peer1 tries to fetch the following properties on peer2:
 * Version
 * Identity
 * Manifest
 * IdentityCertificateId
 * PolicyVersion
 * Policy
 * DefaultPolicy
 * MembershipSummaries
 *
 * Verify:
 * The method calls and Get property calls should fail peer2. bus is not
 * yet claimed.
 */
TEST(SecurityManagementPolicy2Test, ManagedApplication_method_calls_should_fail_before_claim)
{
    BusAttachment peer1("bus1");
    BusAttachment peer2("bus2");

    EXPECT_EQ(ER_OK, peer1.Start());
    EXPECT_EQ(ER_OK, peer1.Connect());
    EXPECT_EQ(ER_OK, peer2.Start());
    EXPECT_EQ(ER_OK, peer2.Connect());

    InMemoryKeyStoreListener bus1KeyStoreListener;
    InMemoryKeyStoreListener bus2KeyStoreListener;

    // Register in memory keystore listeners
    EXPECT_EQ(ER_OK, peer1.RegisterKeyStoreListener(bus1KeyStoreListener));
    EXPECT_EQ(ER_OK, peer2.RegisterKeyStoreListener(bus2KeyStoreListener));

    SecurityManagementPolicy2AuthListener bus1AuthListener;
    SecurityManagementPolicy2AuthListener bus2AuthListener;
    SecurityManagementTestConfigurationListener bus1ConfigurationListener;
    SecurityManagementTestConfigurationListener bus2ConfigurationListener;

    EXPECT_EQ(ER_OK, peer1.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", &bus1AuthListener, nullptr, false, &bus1ConfigurationListener));
    EXPECT_EQ(ER_OK, peer2.EnablePeerSecurity("ALLJOYN_ECDHE_ECDSA", &bus2AuthListener, nullptr, false, &bus2ConfigurationListener));

    SessionOpts opts;
    SessionPort sessionPort = 42;
    SecurityManagementTestSessionPortListener sessionPortListener;
    EXPECT_EQ(ER_OK, peer2.BindSessionPort(sessionPort, opts, sessionPortListener));

    uint32_t sessionId;
    EXPECT_EQ(ER_OK, peer1.JoinSession(peer2.GetUniqueName().c_str(), sessionPort, NULL, sessionId, opts));

    SecurityApplicationProxy sapWithBus1toSelf(peer1, peer1.GetUniqueName().c_str());
    PermissionConfigurator::ApplicationState applicationStateManager;
    EXPECT_EQ(ER_OK, sapWithBus1toSelf.GetApplicationState(applicationStateManager));
    EXPECT_EQ(PermissionConfigurator::NOT_CLAIMABLE, applicationStateManager);

    {
        SecurityApplicationProxy sapBus1toBus2(peer1, peer2.GetUniqueName().c_str(), sessionId);
        PermissionConfigurator::ApplicationState applicationStatePeer1;
        EXPECT_EQ(ER_OK, sapBus1toBus2.GetApplicationState(applicationStatePeer1));
        EXPECT_EQ(PermissionConfigurator::NOT_CLAIMABLE, applicationStatePeer1);

        // Management methods should fail
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.StartManagement());
        EXPECT_EQ(ER_OK, sapBus1toBus2.GetApplicationState(applicationStatePeer1));
        EXPECT_EQ(PermissionConfigurator::NOT_CLAIMABLE, applicationStatePeer1);

        bus2ConfigurationListener.startManagementReceived = false;
        bus2ConfigurationListener.endManagementReceived = false;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.EndManagement());
        EXPECT_EQ(ER_OK, sapBus1toBus2.GetApplicationState(applicationStatePeer1));
        EXPECT_EQ(PermissionConfigurator::NOT_CLAIMABLE, applicationStatePeer1);
        EXPECT_FALSE(bus2ConfigurationListener.startManagementReceived);
        EXPECT_FALSE(bus2ConfigurationListener.endManagementReceived);

        bus2ConfigurationListener.startManagementReceived = false;
        bus2ConfigurationListener.endManagementReceived = false;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.Reset());
        EXPECT_EQ(ER_OK, sapBus1toBus2.GetApplicationState(applicationStatePeer1));
        EXPECT_EQ(PermissionConfigurator::NOT_CLAIMABLE, applicationStatePeer1);
        EXPECT_FALSE(bus2ConfigurationListener.startManagementReceived);
        EXPECT_FALSE(bus2ConfigurationListener.endManagementReceived);

        // Call UpdateIdentity
        Manifest manifests[1];
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateAllInclusiveManifest(manifests[0]));


        // Get manager key
        KeyInfoNISTP256 bus1Key;
        PermissionConfigurator& pcBus1 = peer1.GetPermissionConfigurator();
        EXPECT_EQ(ER_OK, pcBus1.GetSigningPublicKey(bus1Key));

        // Create identityCert
        const size_t certChainSize = 1;
        IdentityCertificate identityCertChain[certChainSize];
        GUID128 guid;


        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateIdentityCert(peer1,
                                                                      "0",
                                                                      guid.ToString(),
                                                                      bus1Key.GetPublicKey(),
                                                                      "Alias",
                                                                      3600,
                                                                      identityCertChain[0])) << "Failed to create identity certificate.";

        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::SignManifest(peer1, identityCertChain[0], manifests[0]));
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.UpdateIdentity(identityCertChain, certChainSize, manifests, ArraySize(manifests)));
        EXPECT_EQ(ER_OK, sapBus1toBus2.SecureConnection(true));

        // Call UpdatePolicy
        PermissionPolicy policy;
        sapBus1toBus2.GetDefaultPolicy(policy);
        policy.SetVersion(1);
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.UpdatePolicy(policy));

        // Call ResetPolicy
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.ResetPolicy());

        // Call InstallMembership
        qcc::MembershipCertificate membershipCertificate[1];
        EXPECT_EQ(ER_OK, PermissionMgmtTestHelper::CreateMembershipCert("1",
                                                                        peer1,
                                                                        peer1.GetUniqueName(),
                                                                        bus1Key.GetPublicKey(),
                                                                        guid,
                                                                        false,
                                                                        3600,
                                                                        membershipCertificate[0]
                                                                        ));
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.InstallMembership(membershipCertificate, 1));
        // Call RemoveMembership
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.RemoveMembership("1", bus1Key));
    }

    {
        SecurityApplicationProxy sapBus1toBus2(peer1, peer2.GetUniqueName().c_str(), sessionId);
        sapBus1toBus2.SecureConnection(true);
        // If ECDHE_ECDSA security is not established none of the method calls
        // will succeed.
        ASSERT_TRUE(bus2AuthListener.authenticationSuccessfull);

        // Fetch Version property
        uint16_t version;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.GetManagedApplicationVersion(version));

        // Fetch Identity property
        MsgArg identityCertificate;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.GetIdentity(identityCertificate));

        // Fetch Manifest property
        std::vector<Manifest> manifests;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.GetManifests(manifests));

        // Fetch IdentityCertificateId property
        String serial;
        qcc::KeyInfoNISTP256 issuerKey;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.GetIdentityCertificateId(serial, issuerKey));

        // Fetch PolicyVersion property
        uint32_t policyVersion;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.GetPolicyVersion(policyVersion));

        // Fetch Policy property
        PermissionPolicy policy;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.GetPolicy(policy));

        // Fetch DefaultPolicy property
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.GetDefaultPolicy(policy));

        // Fetch MembershipSummaries property
        MsgArg membershipSummaries;
        EXPECT_EQ(ER_PERMISSION_DENIED, sapBus1toBus2.GetMembershipSummaries(membershipSummaries));
    }

    // Remove the session port listener allocated on the stack, before it gets destroyed
    EXPECT_EQ(ER_OK, peer2.UnbindSessionPort(sessionPort));
}

/*
 * Verify org.alljoyn.Bus.Security.ManagedApplication interface read only values
 * are read only.
 *
 * These properties should be read only:
 * Version
 * Identity
 * Manifest
 * IdentityCertificateId
 * PolicyVersion
 * Policy
 * DefaultPolicy
 * MembershipSummaries
 */
TEST(SecurityManagementPolicy2Test, verify_values_are_readonly) {
    BusAttachment bus("verify_values_are_readonly");
    const InterfaceDescription* managedAppIface = bus.GetInterface(org::alljoyn::Bus::Security::ManagedApplication::InterfaceName);
    EXPECT_EQ(PROP_ACCESS_READ, managedAppIface->GetProperty("Version")->access);
    EXPECT_EQ(PROP_ACCESS_READ, managedAppIface->GetProperty("Identity")->access);
    EXPECT_EQ(PROP_ACCESS_READ, managedAppIface->GetProperty("Manifests")->access);
    EXPECT_EQ(PROP_ACCESS_READ, managedAppIface->GetProperty("IdentityCertificateId")->access);
    EXPECT_EQ(PROP_ACCESS_READ, managedAppIface->GetProperty("PolicyVersion")->access);
    EXPECT_EQ(PROP_ACCESS_READ, managedAppIface->GetProperty("Policy")->access);
    EXPECT_EQ(PROP_ACCESS_READ, managedAppIface->GetProperty("DefaultPolicy")->access);
    EXPECT_EQ(PROP_ACCESS_READ, managedAppIface->GetProperty("MembershipSummaries")->access);
}
