/*
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of
    the License or (at your option) version 3 or any later version
    accepted by the membership of KDE e.V. (or its successor approved
    by the membership of KDE e.V.), which shall act as a proxy
    defined in Section 14 of version 3 of the license.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "wireguard.h"
#include "wireguardutils.h"

#include <QLatin1Char>
#include <QStringBuilder>
#include <QFile>
#include <QFileInfo>
#include <KPluginFactory>
#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardDirs>

#include <NetworkManagerQt/Connection>
#include <NetworkManagerQt/VpnSetting>
#include <NetworkManagerQt/Ipv4Setting>

#include "wireguardwidget.h"
#include "wireguardauth.h"

#include <arpa/inet.h>

#include "nm-wireguard-service.h"

K_PLUGIN_FACTORY_WITH_JSON(WireGuardUiPluginFactory, "plasmanetworkmanagement_wireguardui.json", registerPlugin<WireGuardUiPlugin>();)

#define NMV_WG_TAG_INTERFACE             "[Interface]"
#define NMV_WG_TAG_PRIVATE_KEY           "PrivateKey"
#define NMV_WG_TAG_LISTEN_PORT           "ListenPort"
#define NMV_WG_TAG_ADDRESS               "Address"
#define NMV_WG_TAG_DNS                   "DNS"
#define NMV_WG_TAG_MTU                   "MTU"
#define NMV_WG_TAG_TABLE                 "Table"
#define NMV_WG_TAG_PRE_UP                "PreUp"
#define NMV_WG_TAG_POST_UP               "PostUp"
#define NMV_WG_TAG_PRE_DOWN              "PreDown"
#define NMV_WG_TAG_POST_DOWN             "PostDown"
#define NMV_WG_ASSIGN                    "="

#define NMV_WG_TAG_PEER                  "[Peer]"
#define NMV_WG_TAG_PUBLIC_KEY            "PublicKey"
#define NMV_WG_TAG_ALLOWED_IPS           "AllowedIPs"
#define NMV_WG_TAG_ENDPOINT              "Endpoint"
#define NMV_WG_TAG_PRESHARED_KEY         "PresharedKey"


WireGuardUiPlugin::WireGuardUiPlugin(QObject * parent, const QVariantList &) : VpnUiPlugin(parent)
{
}

WireGuardUiPlugin::~WireGuardUiPlugin()
{
}

SettingWidget * WireGuardUiPlugin::widget(const NetworkManager::VpnSetting::Ptr &setting, QWidget * parent)
{
    WireGuardSettingWidget * wid = new WireGuardSettingWidget(setting, parent);
    return wid;
}

SettingWidget * WireGuardUiPlugin::askUser(const NetworkManager::VpnSetting::Ptr &setting, QWidget * parent)
{
    return new WireGuardAuthWidget(setting, parent);
}

QString WireGuardUiPlugin::suggestedFileName(const NetworkManager::ConnectionSettings::Ptr &connection) const
{
    return connection->id() + "_wireguard.conf";
}

QString WireGuardUiPlugin::supportedFileExtensions() const
{
    return "*.conf";
}

NMVariantMapMap WireGuardUiPlugin::importConnectionSettings(const QString &fileName)
{
    NMVariantMapMap result;

    QFile impFile(fileName);
    if (!impFile.open(QFile::ReadOnly|QFile::Text)) {
        mError = VpnUiPlugin::Error;
        mErrorMessage = i18n("Could not open file");
        return result;
    }

    const QString connectionName = QFileInfo(fileName).completeBaseName();
    NMStringMap dataMap;
    QVariantMap ipv4Data;

    QString proxy_type;
    QString proxy_user;
    QString proxy_passwd;
    bool have_address = false;
    bool have_private_key = false;
    bool have_public_key = false;
    bool have_allowed_ips = false;
    bool have_endpoint = false;

    QTextStream in(&impFile);
    enum {IDLE, INTERFACE_SECTION, PEER_SECTION} current_state = IDLE;

    while (!in.atEnd()) {
        QStringList key_value;
        QString line = in.readLine();

        // Ignore blank lines
        if (line.isEmpty()) {
            continue;
        }
        key_value.clear();
        key_value << line.split(QRegExp("\\s+=\\s*")); // Split on the ' = '

        if (key_value[0] == NMV_WG_TAG_INTERFACE)
        {
            if (current_state == IDLE)
            {
                current_state = INTERFACE_SECTION;
                continue;
            }
            else
            {
                // BAA - ERROR
                break;
            }
        }

        else if (key_value[0] == NMV_WG_TAG_PEER)
        {
            // Currently only on PEER section is allowed
            if (current_state == INTERFACE_SECTION)
            {
                current_state = PEER_SECTION;
                continue;
            }
            else
            {
                // BAA - ERROR
                break;
            }
        }

        // If we didn't get an '=' sign in the line, it's probably an error but
        // we're going to treat it as a comment and ignore it
        if (key_value.length() < 2)
            continue;

        // If we are in the [Interface] section look for the possible tags
        if (current_state == INTERFACE_SECTION)
        {
            // Address
            if (key_value[0] == NMV_WG_TAG_ADDRESS)
            {
                QStringList address_list;
                address_list << key_value[1].split(QRegExp("\\s*,\\s*"));
                for (int i = 0;i < address_list.size(); i++)
                {
                    if (WireGuardUtils::is_ip4(address_list[i]))
                    {
                        dataMap.insert(QLatin1String(NM_WG_KEY_ADDR_IP4), address_list[i]);
                        have_address = true;
                    }
                    else if (WireGuardUtils::is_ip6(address_list[i]))
                    {
                        dataMap.insert(QLatin1String(NM_WG_KEY_ADDR_IP6), address_list[i]);
                        have_address = true;
                    }
                }
            }

            // Listen Port
            else if (key_value[0] == NMV_WG_TAG_LISTEN_PORT)
            {
                if (WireGuardUtils::is_num_valid(key_value[1], 0, 65535))
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_LISTEN_PORT), key_value[1]);
                }
            }
            // Private Key
            else if (key_value[0] == NMV_WG_TAG_PRIVATE_KEY)
            {
                if (key_value[1].length() > 0)
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_PRIVATE_KEY), key_value[1]);
                    have_private_key = true;
                }
            }
            // DNS
            else if (key_value[0] == NMV_WG_TAG_DNS)
            {
                if (WireGuardUtils::is_ip4(key_value[1]))
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_DNS), key_value[1]);
                }
            }
            // MTU
            else if (key_value[0] == NMV_WG_TAG_MTU)
            {
                if (WireGuardUtils::is_num_valid(key_value[1]))
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_MTU), key_value[1]);
                }
            }
            // Table
            else if (key_value[0] == NMV_WG_TAG_TABLE)
            {
                if (WireGuardUtils::is_num_valid(key_value[1]))
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_TABLE), key_value[1]);
                }
            }
            // PreUp, PostUp, PreDown and PostDown are just ignored because
            // that will be handled by the Network Manager scripts rather than
            // wg-quick
            else if (key_value[0] == NMV_WG_TAG_PRE_UP    ||
                     key_value[0] == NMV_WG_TAG_POST_UP   ||
                     key_value[0] == NMV_WG_TAG_PRE_DOWN  ||
                     key_value[0] == NMV_WG_TAG_POST_DOWN)
            {
                // TODO: maybe add these back in
            }
            else
            {
                // We got a wrong field in the Interface section so it
                // is an error
                break;
            }
        }
        else if (current_state == PEER_SECTION)
        {
            // Public Key
            if (key_value[0] == NMV_WG_TAG_PUBLIC_KEY)
            {
                if (key_value[1].length() > 0)
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_PUBLIC_KEY), key_value[1]);
                    have_public_key = true;
                }
            }
            // Allowed IPs
            else if (key_value[0] == NMV_WG_TAG_ALLOWED_IPS)
            {
                if (key_value[1].length() > 0)
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_ALLOWED_IPS), key_value[1]);
                    have_allowed_ips = true;
                }
            }
            // Endpoint
            else if (key_value[0] == NMV_WG_TAG_ENDPOINT)
            {
                if (key_value[1].length() > 0)
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_ENDPOINT), key_value[1]);
                    have_endpoint = true;
                }
            }
            // Preshared Key
            else if (key_value[0] == NMV_WG_TAG_PRESHARED_KEY)
            {
                if (key_value[1].length() > 0)
                {
                    dataMap.insert(QLatin1String(NM_WG_KEY_PRESHARED_KEY), key_value[1]);
                }
            }
        }
        else   // We're in IDLE or unknown state so it's an error
        {
            // TODO - add error handling
            break;
        }
    }
    if (!have_address || !have_private_key || !have_public_key || !have_endpoint || !have_allowed_ips)
    {

        mError = VpnUiPlugin::Error;
        mErrorMessage = i18n("File %1 is not a valid WireGuard configuration (no remote).", fileName);
        return result;
    }

    NetworkManager::VpnSetting setting;
    setting.setServiceType(QLatin1String(NM_DBUS_SERVICE_WIREGUARD));
    setting.setData(dataMap);

    QVariantMap conn;
    conn.insert("id", connectionName);
    conn.insert("type", "vpn");
    result.insert("connection", conn);
    result.insert("vpn", setting.toMap());

    impFile.close();
    return result;
}

QString WireGuardUiPlugin::saveFile(QTextStream &in, const QString &endTag, const QString &connectionName, const QString &fileName)
{
    const QString certificatesDirectory = KStandardDirs::locateLocal("data", "networkmanagement/certificates/" + connectionName);
    const QString absoluteFilePath = certificatesDirectory + '/' + fileName;
#if 0
    QFile outFile(absoluteFilePath);

    QDir().mkpath(certificatesDirectory);
    if (!outFile.open(QFile::WriteOnly | QFile::Text)) {
        KMessageBox::information(0, i18n("Error saving file %1: %2", absoluteFilePath, outFile.errorString()));
        return QString();
    }

    QTextStream out(&outFile);
    while (!in.atEnd()) {
        const QString line = in.readLine();

        if (line.indexOf(endTag) >= 0) {
            break;
        }

        out << line << "\n";
    }

    outFile.close();
#endif
    return absoluteFilePath;
}

bool WireGuardUiPlugin::exportConnectionSettings(const NetworkManager::ConnectionSettings::Ptr &connection, const QString &fileName)
{
#if 0
    QFile expFile(fileName);
    if (! expFile.open(QIODevice::WriteOnly | QIODevice::Text) ) {
        mError = VpnUiPlugin::Error;
        mErrorMessage = i18n("Could not open file for writing");
        return false;
    }

    NMStringMap dataMap;
    NMStringMap secretData;

    NetworkManager::VpnSetting::Ptr vpnSetting = connection->setting(NetworkManager::Setting::Vpn).dynamicCast<NetworkManager::VpnSetting>();
    dataMap = vpnSetting->data();
    secretData = vpnSetting->secrets();

    QString line;
    QString cacert, user_cert, private_key;

    line = QString(CLIENT_TAG) + '\n';
    expFile.write(line.toLatin1());

    line = QString(REMOTE_TAG) + ' ' + dataMap[NM_OPENVPN_KEY_REMOTE] +
           (dataMap[NM_OPENVPN_KEY_PORT].isEmpty() ? "\n" : (' ' + dataMap[NM_OPENVPN_KEY_PORT]) + '\n');
    expFile.write(line.toLatin1());
    if (dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_TLS ||
            dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_PASSWORD ||
            dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_PASSWORD_TLS) {
        if (!dataMap[NM_OPENVPN_KEY_CA].isEmpty()) {
            cacert = dataMap[NM_OPENVPN_KEY_CA];
        }
    }
    if (dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_TLS ||
            dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_PASSWORD_TLS) {
        if (!dataMap[NM_OPENVPN_KEY_CERT].isEmpty()) {
            user_cert = dataMap[NM_OPENVPN_KEY_CERT];
        } if (!dataMap[NM_OPENVPN_KEY_KEY].isEmpty()) {
            private_key = dataMap[NM_OPENVPN_KEY_KEY];
        }

    }
    // Handle PKCS#12 (all certs are the same file)
    if (!cacert.isEmpty() && !user_cert.isEmpty() && !private_key.isEmpty()
                          && cacert == user_cert && cacert == private_key) {
        line = QString("%1 \"%2\"\n").arg(PKCS12_TAG, cacert);
        expFile.write(line.toLatin1());
    } else {
        if (!cacert.isEmpty()) {
            line = QString("%1 \"%2\"\n").arg(CA_TAG, cacert);
            expFile.write(line.toLatin1());
        }
        if (!user_cert.isEmpty()) {
            line = QString("%1 \"%2\"\n").arg(CERT_TAG, user_cert);
            expFile.write(line.toLatin1());
        }
        if (!private_key.isEmpty()) {
            line = QString("%1 \"%2\"\n").arg(KEY_TAG, private_key);
            expFile.write(line.toLatin1());
        }
    }
    if (dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_TLS ||
        dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_STATIC_KEY ||
        dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_PASSWORD ||
        dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_PASSWORD_TLS) {
        line = QString(AUTH_USER_PASS_TAG) + '\n';
        expFile.write(line.toLatin1());
        if (!dataMap[NM_OPENVPN_KEY_TLS_REMOTE].isEmpty()) {
            line = QString(TLS_REMOTE_TAG) + " \"" + dataMap[NM_OPENVPN_KEY_TLS_REMOTE] + "\"\n";
            expFile.write(line.toLatin1());
        }
        if (!dataMap[NM_OPENVPN_KEY_TA].isEmpty()) {
            line = QString(TLS_AUTH_TAG) + " \"" + dataMap[NM_OPENVPN_KEY_TA] + '\"' + (dataMap[NM_OPENVPN_KEY_TA_DIR].isEmpty() ?
                                "\n" : (' ' + dataMap[NM_OPENVPN_KEY_TA_DIR]) + '\n');
            expFile.write(line.toLatin1());
        }
    }
    if (dataMap[NM_OPENVPN_KEY_CONNECTION_TYPE] == NM_OPENVPN_CONTYPE_STATIC_KEY) {
        line = QString(SECRET_TAG) + " \"" + dataMap[NM_OPENVPN_KEY_STATIC_KEY] + '\"' + (dataMap[NM_OPENVPN_KEY_STATIC_KEY_DIRECTION].isEmpty() ?
                          "\n" : (' ' + dataMap[NM_OPENVPN_KEY_STATIC_KEY_DIRECTION]) + '\n');
        expFile.write(line.toLatin1());
    }
    if (dataMap.contains(NM_OPENVPN_KEY_RENEG_SECONDS) && !dataMap[NM_OPENVPN_KEY_RENEG_SECONDS].isEmpty()) {
        line = QString(RENEG_SEC_TAG) + ' ' + dataMap[NM_OPENVPN_KEY_RENEG_SECONDS] + '\n';
        expFile.write(line.toLatin1());
    }
    if (!dataMap[NM_OPENVPN_KEY_CIPHER].isEmpty()) {
        line = QString(CIPHER_TAG) + ' ' + dataMap[NM_OPENVPN_KEY_CIPHER] + '\n';
        expFile.write(line.toLatin1());
    }
    if (dataMap[NM_OPENVPN_KEY_COMP_LZO] == "yes") {
        line = QString(COMP_TAG) + " yes\n";
        expFile.write(line.toLatin1());
    }
    if (dataMap[NM_OPENVPN_KEY_MSSFIX] == "yes") {
        line = QString(MSSFIX_TAG) + '\n';
        expFile.write(line.toLatin1());
    }
    if (!dataMap[NM_OPENVPN_KEY_TUNNEL_MTU].isEmpty()) {
        line = QString(TUNMTU_TAG) + ' ' + dataMap[NM_OPENVPN_KEY_TUNNEL_MTU] + '\n';
        expFile.write(line.toLatin1());
    }
    if (!dataMap[NM_OPENVPN_KEY_FRAGMENT_SIZE].isEmpty()) {
        line = QString(FRAGMENT_TAG) + ' ' + dataMap[NM_OPENVPN_KEY_FRAGMENT_SIZE] + '\n';
        expFile.write(line.toLatin1());
    }
    line = QString(DEV_TAG) + (dataMap[NM_OPENVPN_KEY_TAP_DEV] == "yes" ? " tap\n" : " tun\n");
    expFile.write(line.toLatin1());
    line = QString(PROTO_TAG) + (dataMap[NM_OPENVPN_KEY_PROTO_TCP] == "yes" ? " tcp\n" : " udp\n");
    expFile.write(line.toLatin1());
    // Proxy stuff
    if (!dataMap[NM_OPENVPN_KEY_PROXY_TYPE].isEmpty()) {
        QString proxy_port = dataMap[NM_OPENVPN_KEY_PROXY_PORT];
        if (dataMap[NM_OPENVPN_KEY_PROXY_TYPE] == "http" && !dataMap[NM_OPENVPN_KEY_PROXY_SERVER].isEmpty()
                                                         && dataMap.contains(NM_OPENVPN_KEY_PROXY_PORT)) {
            if (proxy_port.toInt() == 0) {
                proxy_port = "8080";
            }
            line = QString(HTTP_PROXY_TAG) + ' ' + dataMap[NM_OPENVPN_KEY_PROXY_SERVER] + ' ' + proxy_port +
                    (dataMap[NM_OPENVPN_KEY_HTTP_PROXY_USERNAME].isEmpty() ? "\n" : (' ' + fileName + "-httpauthfile") + '\n');
            expFile.write(line.toLatin1());
            if (dataMap[NM_OPENVPN_KEY_PROXY_RETRY] == "yes") {
                line = QString(HTTP_PROXY_RETRY_TAG) + '\n';
                expFile.write(line.toLatin1());
            }
            // If there is a username, need to write an authfile
            if (!dataMap[NM_OPENVPN_KEY_HTTP_PROXY_USERNAME].isEmpty()) {
                QFile authFile(fileName + "-httpauthfile");
                if (authFile.open(QFile::WriteOnly | QFile::Text)) {
                    line = dataMap[NM_OPENVPN_KEY_HTTP_PROXY_USERNAME] + (dataMap[NM_OPENVPN_KEY_HTTP_PROXY_PASSWORD].isEmpty()?
                                                                         "\n" : (dataMap[NM_OPENVPN_KEY_HTTP_PROXY_PASSWORD] + '\n'));
                    authFile.write(line.toLatin1());
                    authFile.close();
                }
            }
        } else if (dataMap[NM_OPENVPN_KEY_PROXY_TYPE] == "socks" && !dataMap[NM_OPENVPN_KEY_PROXY_SERVER].isEmpty() && dataMap.contains(NM_OPENVPN_KEY_PROXY_PORT)) {
            if (proxy_port.toInt() == 0) {
                proxy_port = "1080";
            }
            line = QString(SOCKS_PROXY_TAG) + dataMap[NM_OPENVPN_KEY_PROXY_SERVER] + ' ' + proxy_port + '\n';
            expFile.write(line.toLatin1());
            if (dataMap[NM_OPENVPN_KEY_PROXY_RETRY] == "yes") {
                line = QString(SOCKS_PROXY_RETRY_TAG) + '\n';
                expFile.write(line.toLatin1());
            }
        }
    }

    NetworkManager::Ipv4Setting::Ptr ipv4Setting = connection->setting(NetworkManager::Setting::Ipv4).dynamicCast<NetworkManager::Ipv4Setting>();
    // Export X-NM-Routes
    if (!ipv4Setting->routes().isEmpty()) {
        QString routes;
        Q_FOREACH (const NetworkManager::IpRoute &route, ipv4Setting->routes()) {
            routes += route.ip().toString() % QLatin1Char('/') % QString::number(route.prefixLength()) % QLatin1Char(' ');
        }
        if (!routes.isEmpty()) {
            routes = "X-NM-Routes " + routes.trimmed();
            expFile.write(routes.toLatin1() + '\n');
        }
    }
    // Add hard-coded stuff
    expFile.write("nobind\n"
                  "auth-nocache\n"
                  "script-security 2\n"
                  "persist-key\n"
                  "persist-tun\n"
                  "user nobody\n"
                  "group nobody\n");
    expFile.close();
#endif
    return true;
}

#include "wireguard.moc"
