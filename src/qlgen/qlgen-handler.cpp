#include <vector>
#include <mutex>
#include <memory>

#include <QDir>
#include <QFileInfo>

#include "seadrive-gui.h"
#include "rpc/rpc-client.h"
#include "utils/utils.h"
#include "utils/file-utils.h"

#include "qlgen/qlgen-handler.h"

namespace {
struct QtLaterDeleter {
public:
  void operator()(QObject *ptr) {
    ptr->deleteLater();
  }
};

bool parseFilePath(const QString &path,
                   QString *repo,
                   QString *path_in_repo,
                   QString *category_out)
{
    // The path of the file in relative to the mount point.
    // It is like "My Libraries/Documents"
    QString relative_path = path.mid(gui->mountDir().length() + 1);

    if (relative_path.isEmpty()) {
        return false;
    }

    if (relative_path.endsWith("/")) {
        relative_path = relative_path.left(relative_path.length() - 1);
    }

    // printf("relative_path is %s\n", toCStr(relative_path));

    if (!category_out && !relative_path.contains('/')) {
        return false;
    }

    int pos = relative_path.indexOf('/');
    QString category = relative_path.left(pos);
    if (category_out) {
        *category_out = category;
    }

    if (!relative_path.contains('/')) {
        return true;
    }

    QString remaining = relative_path.mid(pos + 1);
    // printf("category = %s, remaining = %s\n", category.toUtf8().data(), remaining.toUtf8().data());

    if (remaining.contains('/')) {
        int pos = remaining.indexOf('/');
        *repo = remaining.left(pos);
        *path_in_repo = remaining.mid(pos);
        // printf("repo = %s, path_in_repo = %s\n", repo->toUtf8().data(),
        //        path_in_repo->toUtf8().data());
    } else {
        *repo = remaining;
        *path_in_repo = "";
    }
    return true;
}

// If `category_out` is non-null, repo and path_in_repo would not be used.
bool getRepoAndRelativePath(const QString &path,
                            QString *repo,
                            QString *path_in_repo,
                            QString *category=nullptr)
{
    if (!parseFilePath(path, repo, path_in_repo, category)) {
        return false;
    }
    return !repo->isEmpty();
}

inline QString path_concat(const QString& s1, const QString& s2)
{
    return QString("%1/%2").arg(s1).arg(s2);
}


} // anonymous namespace

QLGenHandler::QLGenHandler() : rpc_client_(new SeafileRpcClient) {
}

void QLGenHandler::start() {
    // TODO: listen to daemon mgr daemonRestarted signal and reconnects to daemon
    rpc_client_->connectDaemon();
}

QLGenHandler::~QLGenHandler() {
}


bool QLGenHandler::isFileCached(const QString &path) {
    QString repo_id;
    QString path_in_repo;
    if (!lookUpFileInformation(path, &repo_id, &path_in_repo)) {
        qWarning("[QLGenHandler] invalid path %s", toCStr(path));
        return false;
    }

    return rpc_client_->isFileCached(repo_id, path_in_repo);
}

bool QLGenHandler::lookUpFileInformation(const QString &path,
                                         QString *ptr_repo_id,
                                           QString *ptr_path_in_repo)
{
    QString repo;
    QString category;
    if (!getRepoAndRelativePath(path, &repo, ptr_path_in_repo, &category)) {
        return false;
    }

    return rpc_client_->getRepoIdByPath(path_concat(category, repo), ptr_repo_id);
}
