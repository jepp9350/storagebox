/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "accountmanager.h"

#include "sslerrordialog.h"
#include "proxyauthhandler.h"
#include "common/asserts.h"
#include "creds/credentialsfactory.h"
#include "creds/abstractcredentials.h"
#include "libsync/clientsideencryption.h"
#include "libsync/configfile.h"
#include "libsync/cookiejar.h"
#include "libsync/theme.h"

#include <QSettings>
#include <QDir>
#include <QNetworkAccessManager>
#include <QMessageBox>

namespace {
constexpr auto urlC = "url";
constexpr auto authTypeC = "authType";
constexpr auto userC = "user";
constexpr auto displayNameC = "displayName";
constexpr auto httpUserC = "http_user";
constexpr auto davUserC = "dav_user";
constexpr auto shibbolethUserC = "shibboleth_shib_user";
constexpr auto caCertsKeyC = "CaCertificates";
constexpr auto accountsC = "Accounts";
constexpr auto versionC = "version";
constexpr auto serverVersionC = "serverVersion";
constexpr auto generalC = "General";

constexpr auto dummyAuthTypeC = "dummy";
constexpr auto httpAuthTypeC = "http";
constexpr auto webflowAuthTypeC = "webflow";
constexpr auto shibbolethAuthTypeC = "shibboleth";
constexpr auto httpAuthPrefix = "http_";
constexpr auto webflowAuthPrefix = "webflow_";

constexpr auto legacyRelativeConfigLocationC = "/ownCloud/owncloud.cfg";
constexpr auto legacyCfgFileNameC = "owncloud.cfg";

// The maximum versions that this client can read
constexpr auto maxAccountsVersion = 2;
constexpr auto maxAccountVersion = 1;
}


namespace OCC {

Q_LOGGING_CATEGORY(lcAccountManager, "nextcloud.gui.account.manager", QtInfoMsg)

AccountManager *AccountManager::instance()
{
    static AccountManager instance;
    return &instance;
}

bool AccountManager::restore(bool alsoRestoreLegacySettings)
{
    QStringList skipSettingsKeys;
    backwardMigrationSettingsKeys(&skipSettingsKeys, &skipSettingsKeys);

    const auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    if (settings->status() != QSettings::NoError || !settings->isWritable()) {
        qCWarning(lcAccountManager) << "Could not read settings from" << settings->fileName()
                                    << settings->status();
        return false;
    }

    if (skipSettingsKeys.contains(settings->group())) {
        // Should not happen: bad container keys should have been deleted
        qCWarning(lcAccountManager) << "Accounts structure is too new, ignoring";
        return true;
    }

    // If there are no accounts, check the old format.
    if (settings->childGroups().isEmpty() && !settings->contains(QLatin1String(versionC)) && alsoRestoreLegacySettings) {
        restoreFromLegacySettings();
        return true;
    }

    const auto settingsChildGroups = settings->childGroups();
    for (const auto &accountId : settingsChildGroups) {
        settings->beginGroup(accountId);
        if (!skipSettingsKeys.contains(settings->group())) {
            if (const auto acc = loadAccountHelper(*settings)) {
                acc->_id = accountId;
                if (auto accState = AccountState::loadFromSettings(acc, *settings)) {
                    auto jar = qobject_cast<CookieJar*>(acc->_am->cookieJar());
                    ASSERT(jar);
                    if (jar)
                        jar->restore(acc->cookieJarPath());
                    addAccountState(accState);
                }
            }
        } else {
            qCInfo(lcAccountManager) << "Account" << accountId << "is too new, ignoring";
            _additionalBlockedAccountIds.insert(accountId);
        }
        settings->endGroup();
    }

    return true;
}

void AccountManager::backwardMigrationSettingsKeys(QStringList *deleteKeys, QStringList *ignoreKeys)
{
    const auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    const auto accountsVersion = settings->value(QLatin1String(versionC)).toInt();

    if (accountsVersion <= maxAccountsVersion) {
        const auto settingsChildGroups = settings->childGroups();
        for (const auto &accountId : settingsChildGroups) {
            settings->beginGroup(accountId);
            const auto accountVersion = settings->value(QLatin1String(versionC), 1).toInt();

            if (accountVersion > maxAccountVersion) {
                ignoreKeys->append(settings->group());
            }
            settings->endGroup();
        }
    } else {
        deleteKeys->append(settings->group());
    }
}

bool AccountManager::restoreFromLegacySettings()
{
    qCInfo(lcAccountManager) << "Migrate: restoreFromLegacySettings, checking settings group"
                             << Theme::instance()->appName();

    // try to open the correctly themed settings
    auto settings = ConfigFile::settingsWithGroup(Theme::instance()->appName());

    // if the settings file could not be opened, the childKeys list is empty
    // then try to load settings from a very old place
    if (settings->childKeys().isEmpty()) {
        // Legacy settings used QDesktopServices to get the location for the config folder in 2.4 and before
        const auto legacy2_4CfgSettingsLocation = QString(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/data"));
        const auto legacy2_4CfgFileParentFolder = legacy2_4CfgSettingsLocation.left(legacy2_4CfgSettingsLocation.lastIndexOf('/'));

        // 2.5+ (rest of 2.x series)
        const auto legacy2_5CfgSettingsLocation = QStandardPaths::writableLocation(Utility::isWindows() ? QStandardPaths::AppDataLocation : QStandardPaths::AppConfigLocation);
        const auto legacy2_5CfgFileParentFolder = legacy2_5CfgSettingsLocation.left(legacy2_5CfgSettingsLocation.lastIndexOf('/'));

        // Now try the locations we use today
        const auto fullLegacyCfgFile = QDir::fromNativeSeparators(settings->fileName());
        const auto legacyCfgFileParentFolder = fullLegacyCfgFile.left(fullLegacyCfgFile.lastIndexOf('/'));
        const auto legacyCfgFileGrandParentFolder = legacyCfgFileParentFolder.left(legacyCfgFileParentFolder.lastIndexOf('/'));

        const auto legacyCfgFileNamePath = QString(QStringLiteral("/") + legacyCfgFileNameC);
        const auto legacyCfgFileRelativePath = QString(legacyRelativeConfigLocationC);

        const auto legacyLocations = QVector<QString>{legacy2_4CfgFileParentFolder + legacyCfgFileRelativePath,
                                                      legacy2_5CfgFileParentFolder + legacyCfgFileRelativePath,
                                                      legacyCfgFileParentFolder + legacyCfgFileNamePath,
                                                      legacyCfgFileGrandParentFolder + legacyCfgFileRelativePath};

        for (const auto &configFile : legacyLocations) {
            if (const QFileInfo configFileInfo(configFile);
                    configFileInfo.exists() && configFileInfo.isReadable()) {

                qCInfo(lcAccountManager) << "Migrate: checking old config " << configFile;

                auto oCSettings = std::make_unique<QSettings>(configFile, QSettings::IniFormat);
                if (oCSettings->status() != QSettings::Status::NoError) {
                    qCInfo(lcAccountManager) << "Error reading legacy configuration file" << oCSettings->status();
                    break;
                }

                // Check the theme url to see if it is the same url that the oC config was for
                const auto overrideUrl = Theme::instance()->overrideServerUrl();
                const auto cleanOverrideUrl = overrideUrl.endsWith('/') ? overrideUrl.chopped(1) : overrideUrl;
                qCInfo(lcAccountManager) << "Migrate: overrideUrl" << cleanOverrideUrl;

                if (!cleanOverrideUrl.isEmpty()) {
                    oCSettings->beginGroup(QLatin1String(accountsC));
                    const auto accountsChildGroups = oCSettings->childGroups();

                    for (const auto &accountId : accountsChildGroups) {
                        oCSettings->beginGroup(accountId);
                        const auto oCUrl = oCSettings->value(QLatin1String(urlC)).toString();
                        const auto cleanOCUrl = oCUrl.endsWith('/') ? oCUrl.chopped(1) : oCUrl;

                        // in case the urls are equal reset the settings object to read from
                        // the ownCloud settings object
                        qCInfo(lcAccountManager) << "Migrate oC config if " << cleanOCUrl << " == " << cleanOverrideUrl << ":"
                                                 << (cleanOCUrl == cleanOverrideUrl ? "Yes" : "No");
                        if (cleanOCUrl == cleanOverrideUrl) {
                            qCInfo(lcAccountManager) << "Copy settings" << oCSettings->allKeys().join(", ");
                            oCSettings->endGroup(); // current accountID group
                            oCSettings->endGroup(); // accounts group
                            settings = std::move(oCSettings);
                            break;
                        }

                        oCSettings->endGroup();
                    }

                    if (oCSettings) {
                        oCSettings->endGroup();
                    }
                } else {
                    qCInfo(lcAccountManager) << "Copy settings" << oCSettings->allKeys().join(", ");
                    settings = std::move(oCSettings);
                }

                break;
            } else {
                qCInfo(lcAccountManager) << "Migrate: could not read old config " << configFile;
            }
        }
    }

    // Try to load the single account.
    if (!settings->childKeys().isEmpty()) {
        settings->beginGroup(accountsC);
        const auto childGroups = settings->childGroups();
        for (const auto &accountId : childGroups) {
            settings->beginGroup(accountId);
            if (const auto acc = loadAccountHelper(*settings)) {
                addAccount(acc);

                return true;
            }
        }
    }
    return false;
}

void AccountManager::save(bool saveCredentials)
{
    const auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    settings->setValue(QLatin1String(versionC), maxAccountsVersion);
    for (const auto &acc : qAsConst(_accounts)) {
        settings->beginGroup(acc->account()->id());
        saveAccountHelper(acc->account().data(), *settings, saveCredentials);
        settings->endGroup();
    }

    settings->sync();
    qCInfo(lcAccountManager) << "Saved all account settings, status:" << settings->status();
}

void AccountManager::saveAccount(Account *a)
{
    qCDebug(lcAccountManager) << "Saving account" << a->url().toString();
    const auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    settings->beginGroup(a->id());
    saveAccountHelper(a, *settings, false); // don't save credentials they might not have been loaded yet
    settings->endGroup();

    settings->sync();
    qCDebug(lcAccountManager) << "Saved account settings, status:" << settings->status();
}

void AccountManager::saveAccountState(AccountState *a)
{
    qCDebug(lcAccountManager) << "Saving account state" << a->account()->url().toString();
    const auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    settings->beginGroup(a->account()->id());
    settings->endGroup();

    settings->sync();
    qCDebug(lcAccountManager) << "Saved account state settings, status:" << settings->status();
}

void AccountManager::saveAccountHelper(Account *acc, QSettings &settings, bool saveCredentials)
{
    settings.setValue(QLatin1String(versionC), maxAccountVersion);
    settings.setValue(QLatin1String(urlC), acc->_url.toString());
    settings.setValue(QLatin1String(davUserC), acc->_davUser);
    settings.setValue(QLatin1String(displayNameC), acc->_displayName);
    settings.setValue(QLatin1String(serverVersionC), acc->_serverVersion);

    if (acc->_credentials) {
        if (saveCredentials) {
            // Only persist the credentials if the parameter is set, on migration from 1.8.x
            // we want to save the accounts but not overwrite the credentials
            // (This is easier than asynchronously fetching the credentials from keychain and then
            // re-persisting them)
            acc->_credentials->persist();
        }

        const auto settingsMapKeys = acc->_settingsMap.keys();
        for (const auto &key : settingsMapKeys) {
            settings.setValue(key, acc->_settingsMap.value(key));
        }
        settings.setValue(QLatin1String(authTypeC), acc->_credentials->authType());

        // HACK: Save http_user also as user
        if (acc->_settingsMap.contains(httpUserC))
            settings.setValue(userC, acc->_settingsMap.value(httpUserC));
    }

    // Save accepted certificates.
    settings.beginGroup(QLatin1String(generalC));
    qCInfo(lcAccountManager) << "Saving " << acc->approvedCerts().count() << " unknown certs.";
    QByteArray certs;
    const auto approvedCerts = acc->approvedCerts();
    for (const auto &cert : approvedCerts) {
        certs += cert.toPem() + '\n';
    }
    if (!certs.isEmpty()) {
        settings.setValue(QLatin1String(caCertsKeyC), certs);
    }
    settings.endGroup();

    // Save cookies.
    if (acc->_am) {
        auto *jar = qobject_cast<CookieJar *>(acc->_am->cookieJar());
        if (jar) {
            qCInfo(lcAccountManager) << "Saving cookies." << acc->cookieJarPath();
            if (!jar->save(acc->cookieJarPath()))
            {
                qCWarning(lcAccountManager) << "Failed to save cookies to" << acc->cookieJarPath();
            }
        }
    }
}

AccountPtr AccountManager::loadAccountHelper(QSettings &settings)
{
    const auto urlConfig = settings.value(QLatin1String(urlC));
    if (!urlConfig.isValid()) {
        // No URL probably means a corrupted entry in the account settings
        qCWarning(lcAccountManager) << "No URL for account " << settings.group();
        return AccountPtr();
    }

    const auto acc = createAccount();

    auto authType = settings.value(QLatin1String(authTypeC)).toString();

    // There was an account-type saving bug when 'skip folder config' was used
    // See #5408. This attempts to fix up the "dummy" authType
    if (authType == QLatin1String(dummyAuthTypeC)) {
        if (settings.contains(QLatin1String(httpUserC))) {
            authType = httpAuthTypeC;
        } else if (settings.contains(QLatin1String(shibbolethUserC))) {
            authType = shibbolethAuthTypeC;
        }
    }

    const auto overrideUrl = Theme::instance()->overrideServerUrl();
    const auto forceAuth = Theme::instance()->forceConfigAuthType();
    if (!forceAuth.isEmpty() && !overrideUrl.isEmpty()) {
        // If forceAuth is set, this might also mean the overrideURL has changed.
        // See enterprise issues #1126
        acc->setUrl(overrideUrl);
        authType = forceAuth;
    } else {
        acc->setUrl(urlConfig.toUrl());
    }

    // Migrate to webflow
    if (authType == QLatin1String(httpAuthTypeC)) {
        authType = webflowAuthTypeC;
        settings.setValue(QLatin1String(authTypeC), authType);

        const auto settingsChildKeys = settings.childKeys();
        for (const auto &key : settingsChildKeys) {
            if (!key.startsWith(httpAuthPrefix))
                continue;
            const auto newkey = QString::fromLatin1(webflowAuthPrefix).append(key.mid(5));
            settings.setValue(newkey, settings.value((key)));
            settings.remove(key);
        }
    }

    qCInfo(lcAccountManager) << "Account for" << acc->url() << "using auth type" << authType;

    acc->_serverVersion = settings.value(QLatin1String(serverVersionC)).toString();
    acc->_davUser = settings.value(QLatin1String(davUserC), "").toString();

    // We want to only restore settings for that auth type and the user value
    acc->_settingsMap.insert(QLatin1String(userC), settings.value(userC));
    acc->_displayName = settings.value(QLatin1String(displayNameC), "").toString();
    QString authTypePrefix = authType + "_";
    const auto settingsChildKeys = settings.childKeys();
    for (const auto &key : settingsChildKeys) {
        if (!key.startsWith(authTypePrefix))
            continue;
        acc->_settingsMap.insert(key, settings.value(key));
    }

    acc->setCredentials(CredentialsFactory::create(authType));

    // now the server cert, it is in the general group
    settings.beginGroup(QLatin1String(generalC));
    const auto certs = QSslCertificate::fromData(settings.value(caCertsKeyC).toByteArray());
    qCInfo(lcAccountManager) << "Restored: " << certs.count() << " unknown certs.";
    acc->setApprovedCerts(certs);
    settings.endGroup();

    return acc;
}

AccountStatePtr AccountManager::account(const QString &name)
{
    const auto it = std::find_if(_accounts.cbegin(), _accounts.cend(), [name](const auto &acc) {
        return acc->account()->displayName() == name;
    });
    return it != _accounts.cend() ? *it : AccountStatePtr();
}

AccountStatePtr AccountManager::accountFromUserId(const QString &id) const
{
    const auto accountsList = accounts();
    for (const auto &account : accountsList) {
        const auto isUserIdWithPort = id.split(QLatin1Char(':')).size() > 1;
        const auto port = isUserIdWithPort ? account->account()->url().port() : -1;
        const auto portString = (port > 0 && port != 80 && port != 443) ? QStringLiteral(":%1").arg(port) : QStringLiteral("");
        const QString davUserId = QStringLiteral("%1@%2").arg(account->account()->davUser(), account->account()->url().host()) + portString;

        if (davUserId == id) {
            return account;
        }
    }
    return {};
}

AccountState *AccountManager::addAccount(const AccountPtr &newAccount)
{
    auto id = newAccount->id();
    if (id.isEmpty() || !isAccountIdAvailable(id)) {
        id = generateFreeAccountId();
    }
    newAccount->_id = id;

    const auto newAccountState = new AccountState(newAccount);
    addAccountState(newAccountState);
    return newAccountState;
}

void AccountManager::deleteAccount(AccountState *account)
{
    const auto it = std::find(_accounts.begin(), _accounts.end(), account);
    if (it == _accounts.end()) {
        return;
    }
    const auto copy = *it; // keep a reference to the shared pointer so it does not delete it just yet
    _accounts.erase(it);

    // Forget account credentials, cookies
    account->account()->credentials()->forgetSensitiveData();
    QFile::remove(account->account()->cookieJarPath());

    const auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    settings->remove(account->account()->id());

    // Forget E2E keys
    account->account()->e2e()->forgetSensitiveData(account->account());

    account->account()->deleteAppToken();

    emit accountSyncConnectionRemoved(account);
    emit accountRemoved(account);
}

AccountPtr AccountManager::createAccount()
{
    const auto acc = Account::create();
    acc->setSslErrorHandler(new SslDialogErrorHandler);
    connect(acc.data(), &Account::proxyAuthenticationRequired,
        ProxyAuthHandler::instance(), &ProxyAuthHandler::handleProxyAuthenticationRequired);
    connect(acc.data(), &Account::lockFileError,
        Systray::instance(), &Systray::showErrorMessageDialog);

    return acc;
}

void AccountManager::shutdown()
{
    const auto accountsCopy = _accounts;
    _accounts.clear();
    for (const auto &acc : accountsCopy) {
        emit accountRemoved(acc.data());
        emit removeAccountFolders(acc.data());
    }
}

QList<AccountStatePtr> AccountManager::accounts() const
{
     return _accounts;
}

bool AccountManager::isAccountIdAvailable(const QString &id) const
{
    if (_additionalBlockedAccountIds.contains(id))
        return false;

    return std::none_of(_accounts.cbegin(), _accounts.cend(), [id](const auto &acc) {
        return acc->account()->id() == id;
    });
}

QString AccountManager::generateFreeAccountId() const
{
    auto i = 0;
    forever {
        const auto id = QString::number(i);
        if (isAccountIdAvailable(id)) {
            return id;
        }
        ++i;
    }
}

void AccountManager::addAccountState(AccountState *accountState)
{
    QObject::connect(accountState->account().data(),
        &Account::wantsAccountSaved,
        this, &AccountManager::saveAccount);

    AccountStatePtr ptr(accountState);
    _accounts << ptr;
    emit accountAdded(accountState);
}
}
