#include <sqlite3.h>
#include <glib.h>
#include <errno.h>
#include <stdio.h>
#include <algorithm>

#include <QDateTime>
#include <QMutexLocker>

#include "account-mgr.h"
#include "seadrive-gui.h"
#include "utils/utils.h"
#include "api/api-error.h"
#include "api/requests.h"
#include "rpc/rpc-client.h"
#include "ui/login-dialog.h"
#include "shib/shib-login-dialog.h"
#include "settings-mgr.h"
#include "account-info-service.h"

#if defined (Q_OS_WIN32)
#include "win-sso/auto-logon-dialog.h"
#endif

namespace {
const char *kVersionKeyName = "version";
const char *kFeaturesKeyName = "features";
const char *kCustomBrandKeyName = "custom-brand";
const char *kCustomLogoKeyName = "custom-logo";
const char *kTotalStorage = "storage.total";
const char *kUsedStorage = "storage.used";
const char *kNickname = "name";

bool getShibbolethColumnInfoCallBack(sqlite3_stmt *stmt, void *data)
{
    bool *has_shibboleth_column = static_cast<bool*>(data);
    const char *column_name = (const char *)sqlite3_column_text (stmt, 1);

    if (0 == strcmp("isShibboleth", column_name))
        *has_shibboleth_column = true;

    return true;
}

bool getKerberosColumnInfoCallBack(sqlite3_stmt *stmt, void *data)
{
    bool *has_kerberos_column = static_cast<bool*>(data);
    const char *column_name = (const char *)sqlite3_column_text (stmt, 1);

    if (0 == strcmp("isKerberos", column_name))
        *has_kerberos_column = true;

    return true;
}

bool getAutomaticLoginColumnInfoCallBack(sqlite3_stmt *stmt, void *data)
{
    bool *has_automatic_login_column = static_cast<bool*>(data);
    const char *column_name = (const char *)sqlite3_column_text (stmt, 1);

    if (0 == strcmp("AutomaticLogin", column_name))
        *has_automatic_login_column = true;

    return true;
}

void updateAccountDatabaseForColumnShibbolethUrl(struct sqlite3* db)
{
    bool has_shibboleth_column = false;
    const char* sql = "PRAGMA table_info(Accounts);";
    sqlite_foreach_selected_row (db, sql, getShibbolethColumnInfoCallBack, &has_shibboleth_column);
    sql = "ALTER TABLE Accounts ADD COLUMN isShibboleth INTEGER";
    if (!has_shibboleth_column && sqlite_query_exec (db, sql) < 0)
        qCritical("unable to create isShibboleth column\n");
}

void updateAccountDatabaseForColumnKerberosUrl(struct sqlite3* db)
{
    bool has_kerberos_column = false;
    const char* sql = "PRAGMA table_info(Accounts);";
    sqlite_foreach_selected_row (db, sql, getKerberosColumnInfoCallBack, &has_kerberos_column);
    sql = "ALTER TABLE Accounts ADD COLUMN isKerberos INTEGER";
    if (!has_kerberos_column && sqlite_query_exec (db, sql) < 0)
        qCritical("unable to create isKerberos column\n");
}

void updateAccountDatabaseForColumnAutomaticLogin(struct sqlite3* db)
{
    bool has_automatic_login_column = false;
    const char* sql = "PRAGMA table_info(Accounts);";
    sqlite_foreach_selected_row (db, sql, getAutomaticLoginColumnInfoCallBack, &has_automatic_login_column);
    sql = "ALTER TABLE Accounts ADD COLUMN AutomaticLogin INTEGER default 1";
    if (!has_automatic_login_column && sqlite_query_exec (db, sql) < 0)
        qCritical("unable to create AutomaticLogin column\n");
}

bool compareAccount(const Account& a, const Account& b)
{
    if (!a.isValid()) {
        return false;
    } else if (!b.isValid()) {
        return true;
    } else if (a.lastVisited < b.lastVisited) {
        return false;
    } else if (a.lastVisited > b.lastVisited) {
        return true;
    }

    return true;
}

struct UserData {
    std::vector<Account> *accounts;
    struct sqlite3 *db;
};

inline void setServerInfoKeyValue(struct sqlite3 *db, const Account &account, const QString& key, const QString &value)
{
    char *zql = sqlite3_mprintf(
        "REPLACE INTO ServerInfo(url, username, key, value) VALUES (%Q, %Q, %Q, %Q)",
        account.serverUrl.toEncoded().data(), account.username.toUtf8().data(),
        key.toUtf8().data(), value.toUtf8().data());
    sqlite_query_exec(db, zql);
    sqlite3_free(zql);
}

}

AccountManager::AccountManager()
{
    db = NULL;
}

AccountManager::~AccountManager()
{
    if (db)
        sqlite3_close(db);
}

int AccountManager::start()
{
    const char *errmsg;
    const char *sql;

    QString db_path = QDir(gui->seadriveDir()).filePath("accounts.db");
    if (sqlite3_open (toCStr(db_path), &db)) {
        errmsg = sqlite3_errmsg (db);
        qCritical("failed to open account database %s: %s",
                toCStr(db_path), errmsg ? errmsg : "no error given");

        gui->errorAndExit(tr("failed to open account database"));
        return -1;
    }

    // enabling foreign keys, it must be done manually from each connection
    // and this feature is only supported from sqlite 3.6.19
    sql = "PRAGMA foreign_keys=ON;";
    if (sqlite_query_exec (db, sql) < 0) {
        qCritical("sqlite version is too low to support foreign key feature\n");
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    sql = "CREATE TABLE IF NOT EXISTS Accounts (url VARCHAR(24), "
        "username VARCHAR(15), token VARCHAR(40), lastVisited INTEGER, "
        "PRIMARY KEY(url, username))";
    if (sqlite_query_exec (db, sql) < 0) {
        qCritical("failed to create accounts table\n");
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    updateAccountDatabaseForColumnShibbolethUrl(db);
    updateAccountDatabaseForColumnAutomaticLogin(db);
    updateAccountDatabaseForColumnKerberosUrl(db);

    // create ServerInfo table
    sql = "CREATE TABLE IF NOT EXISTS ServerInfo ("
        "key TEXT NOT NULL, value TEXT, "
        "url VARCHAR(24), username VARCHAR(15), "
        "PRIMARY KEY(url, username, key), "
        "FOREIGN KEY(url, username) REFERENCES Accounts(url, username) "
        "ON DELETE CASCADE ON UPDATE CASCADE )";
    if (sqlite_query_exec (db, sql) < 0) {
        qCritical("failed to create server_info table\n");
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    loadAccounts();

    connect(this, SIGNAL(accountsChanged()), this, SLOT(onAccountsChanged()));
    return 0;
}

bool AccountManager::loadAccountsCB(sqlite3_stmt *stmt, void *data)
{
    UserData *userdata = static_cast<UserData*>(data);
    const char *url = (const char *)sqlite3_column_text (stmt, 0);
    const char *username = (const char *)sqlite3_column_text (stmt, 1);
    const char *token = (const char *)sqlite3_column_text (stmt, 2);
    qint64 atime = (qint64)sqlite3_column_int64 (stmt, 3);
    int isShibboleth = sqlite3_column_int (stmt, 4);
    int isAutomaticLogin = sqlite3_column_int (stmt, 5);
    int isKerberos = sqlite3_column_int (stmt, 6);

    if (!token) {
        token = "";
    }

    Account account = Account(QUrl(QString(url)), QString(username),
                              QString(token), atime, isShibboleth != 0,
                              isAutomaticLogin != 0, isKerberos != 0);
    char* zql = sqlite3_mprintf("SELECT key, value FROM ServerInfo WHERE url = %Q AND username = %Q", url, username);
    sqlite_foreach_selected_row (userdata->db, zql, loadServerInfoCB, &account);
    sqlite3_free(zql);

    userdata->accounts->push_back(account);
    return true;
}

bool AccountManager::loadServerInfoCB(sqlite3_stmt *stmt, void *data)
{
    Account *account = static_cast<Account*>(data);
    const char *key = (const char *)sqlite3_column_text (stmt, 0);
    const char *value = (const char *)sqlite3_column_text (stmt, 1);
    QString key_string = key;
    QString value_string = value;
    if (key_string == kVersionKeyName) {
        account->serverInfo.parseVersionFromString(value_string);
    } else if (key_string == kFeaturesKeyName) {
        account->serverInfo.parseFeatureFromStrings(value_string.split(","));
    } else if (key_string == kCustomBrandKeyName) {
        account->serverInfo.customBrand = value_string;
    } else if (key_string == kCustomLogoKeyName) {
        account->serverInfo.customLogo = value_string;
    } else if (key_string == kTotalStorage) {
        account->accountInfo.totalStorage = value_string.toLongLong();
    } else if (key_string == kUsedStorage) {
        account->accountInfo.usedStorage = value_string.toLongLong();
    } else if (key_string == kNickname) {
        account->accountInfo.name = value_string;
    }
    return true;
}

const std::vector<Account>& AccountManager::loadAccounts()
{
    const char *sql = "SELECT url, username, token, lastVisited, isShibboleth, AutomaticLogin, isKerberos "
                      "FROM Accounts ORDER BY lastVisited DESC";
    accounts_.clear();
    UserData userdata;
    userdata.accounts = &accounts_;
    userdata.db = db;
    sqlite_foreach_selected_row (db, sql, loadAccountsCB, &userdata);

    std::stable_sort(accounts_.begin(), accounts_.end(), compareAccount);

    qWarning("loaded %d accounts", (int)accounts_.size());

    return accounts_;
}

int AccountManager::saveAccount(const Account& account)
{
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    Account new_account = account;
    new_account.lastVisited = timestamp;
    {
        QMutexLocker lock(&accounts_mutex_);
        for (size_t i = 0; i < accounts_.size(); i++) {
            if (accounts_[i] == account) {
                accounts_.erase(accounts_.begin() + i);
                break;
            }
        }
        accounts_.insert(accounts_.begin(), new_account);
    }
    updateServerInfo(0);

    char *zql = sqlite3_mprintf(
        "REPLACE INTO Accounts(url, username, token, lastVisited, isShibboleth, AutomaticLogin, isKerberos)"
        "VALUES (%Q, %Q, %Q, %Q, %Q, %Q, %Q) ",
        // url
        new_account.serverUrl.toEncoded().data(),
        // username
        new_account.username.toUtf8().data(),
        // token
        new_account.token.toUtf8().data(),
        // lastVisited
        QString::number(timestamp).toUtf8().data(),
        // isShibboleth
        QString::number(new_account.isShibboleth).toUtf8().data(),
        // isAutomaticLogin
        QString::number(new_account.isAutomaticLogin).toUtf8().data(),
        // isKerberos
        QString::number(new_account.isKerberos).toUtf8().data());
    sqlite_query_exec(db, zql);
    sqlite3_free(zql);

    gui->rpcClient()->seafileSetConfig(
        "client_name", gui->settingsManager()->getComputerName());

    emit accountsChanged();

    return 0;
}

int AccountManager::removeAccount(const Account& account)
{
    char *zql = sqlite3_mprintf(
        "DELETE FROM Accounts WHERE url = %Q AND username = %Q",
        // url
        account.serverUrl.toEncoded().data(),
        // username
        account.username.toUtf8().data());
    sqlite_query_exec(db, zql);
    sqlite3_free(zql);

    bool need_switch_account = currentAccount() == account;

    {
        QMutexLocker lock(&accounts_mutex_);
        accounts_.erase(
            std::remove(accounts_.begin(), accounts_.end(), account),
            accounts_.end());
    }

    if (need_switch_account) {
        if (!accounts_.empty()) {
            validateAndUseAccount(accounts_[0]);
        } else {
            LoginDialog login_dialog;
            login_dialog.exec();
        }
    }

    emit accountsChanged();

    return 0;
}

void AccountManager::updateAccountLastVisited(const Account& account)
{
    char *zql = sqlite3_mprintf(
        "UPDATE Accounts SET lastVisited = %Q "
        "WHERE url = %Q AND username = %Q",
        // lastVisted
        QString::number(QDateTime::currentMSecsSinceEpoch()).toUtf8().data(),
        // url
        account.serverUrl.toEncoded().data(),
        // username
        account.username.toUtf8().data());
    sqlite_query_exec(db, zql);
    sqlite3_free(zql);
}

bool AccountManager::accountExists(const QUrl& url, const QString& username)
{
    for (size_t i = 0; i < accounts_.size(); i++) {
        if (accounts_[i].serverUrl == url && accounts_[i].username == username) {
            return true;
        }
    }

    return false;
}

bool AccountManager::validateAndUseAccount(const Account& account)
{
    if (!account.isAutomaticLogin && account.lastVisited < gui->startupTime()) {
        return clearAccountToken(account, true);
    } else if (!account.isValid()) {
        return reloginAccount(account);
    } else {
        return setCurrentAccount(account);
    }
}

bool AccountManager::setCurrentAccount(const Account& account)
{
    Q_ASSERT(account.isValid());

    // Update the account timestamp and emit "accountsChanged" signal
    saveAccount(account);

    if (account == currentAccount()) {
        return false;
    }

    AccountInfoService::instance()->refresh();

    return true;
}

Account AccountManager::getAccountByHostAndUsername(const QString& host,
                                                    const QString& username) const
{
    for (size_t i = 0; i < accounts_.size(); i++) {
        if (accounts_[i].serverUrl.host() == host
            && accounts_[i].username == username) {
            return accounts_[i];
        }
    }

    return Account();
}

Account AccountManager::getAccountBySignature(const QString& account_sig) const
{
    for (size_t i = 0; i < accounts_.size(); i++) {
        if (accounts_[i].getSignature() == account_sig) {
            return accounts_[i];
        }
    }

    return Account();
}

void AccountManager::updateServerInfo(unsigned index)
{
    ServerInfoRequest *request;
    // request is taken owner by Account object
    request = accounts_[index].createServerInfoRequest();
    connect(request, SIGNAL(success(const Account&, const ServerInfo &)),
            this, SLOT(serverInfoSuccess(const Account&, const ServerInfo &)));
    connect(request, SIGNAL(failed(const ApiError&)),
            this, SLOT(serverInfoFailed(const ApiError&)));
    request->send();
}

void AccountManager::updateAccountInfo(const Account& account,
                                       const AccountInfo& info)
{
    setServerInfoKeyValue(db, account, kTotalStorage,
                          QString::number(info.totalStorage));
    setServerInfoKeyValue(db, account, kUsedStorage,
                          QString::number(info.usedStorage));
    setServerInfoKeyValue(db, account, kNickname,
                          info.name);

    for (size_t i = 0; i < accounts_.size(); i++) {
        if (accounts_[i] == account) {
            accounts_[i].accountInfo = info;
            emit accountInfoUpdated(accounts_[i]);
            break;
        }
    }
}


void AccountManager::serverInfoSuccess(const Account &account, const ServerInfo &info)
{
    setServerInfoKeyValue(db, account, kVersionKeyName, info.getVersionString());
    setServerInfoKeyValue(db, account, kFeaturesKeyName, info.getFeatureStrings().join(","));
    setServerInfoKeyValue(db, account, kCustomLogoKeyName, info.customLogo);
    setServerInfoKeyValue(db, account, kCustomBrandKeyName, info.customBrand);

    QUrl url(account.serverUrl);
    url.setPath("/");
    // gui->rpcClient()->setServerProperty(
    //     url.toString(), "is_pro", account.isPro() ? "true" : "false");

    gui->rpcClient()->switchAccount(account, info.proEdition);

    bool changed = account.serverInfo != info;
    if (!changed)
        return;


    for (size_t i = 0; i < accounts_.size(); i++) {
        if (accounts_[i] == account) {
            accounts_[i].serverInfo = info;
            if (i == 0)
                emit accountsChanged();
            break;
        }
    }
}

void AccountManager::serverInfoFailed(const ApiError &error)
{
    qWarning("update server info failed %s\n", error.toString().toUtf8().data());
}

bool AccountManager::clearAccountToken(const Account& account,
                                       bool force_relogin)
{
    for (size_t i = 0; i < accounts_.size(); i++) {
        if (accounts_[i] == account) {
            accounts_[i].token = "";
            break;
        }
    }

    char *zql = sqlite3_mprintf(
        "UPDATE Accounts "
        "SET token = NULL "
        "WHERE url = %Q "
        "  AND username = %Q",
        // url
        account.serverUrl.toEncoded().data(),
        // username
        account.username.toUtf8().data()
        );
    sqlite_query_exec(db, zql);
    sqlite3_free(zql);

    if (force_relogin || account == currentAccount()) {
        reloginAccount(account);
    } else {
        emit accountsChanged();
    }

    return true;
}


void AccountManager::onAccountsChanged()
{
    QMutexLocker cache_lock(&accounts_cache_mutex_);
    accounts_cache_.clear();
}

void AccountManager::invalidateCurrentLogin()
{
    // make sure we have accounts there
    if (!hasAccount())
        return;
    const Account &account = accounts_.front();
    // if the token is already invalidated, ignore
    if (account.token.isEmpty())
        return;

    clearAccountToken(account);
}

bool AccountManager::reloginAccount(const Account &account)
{
    bool accepted;
    do {
        if (account.isShibboleth) {
            ShibLoginDialog shib_dialog(account.serverUrl, gui->settingsManager()->getComputerName());
            accepted = shib_dialog.exec() == QDialog::Accepted;
            break;
        }
#if defined(Q_OS_WIN32)
        if (account.isKerberos) {
            AutoLogonDialog dialog;
            accepted = dialog.exec() == QDialog::Accepted;
            break;
        }
#endif
        LoginDialog dialog;
        dialog.initFromAccount(account);
        accepted = dialog.exec() == QDialog::Accepted;
    } while (0);
    return accepted;
}
