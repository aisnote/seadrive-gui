#ifndef SEADRIVE_CLIENT_QLGEN_HANDLER_H_
#define SEADRIVE_CLIENT_QLGEN_HANDLER_H_
#include <QObject>
#include <QString>
#include "api/api-error.h"

class SeafileRpcClient;

// This class handles thumbnails generation for files under seadrive
// volume.
//
// For now it's only used for MacOS thumbnail generation. In
// the future We may also use this class for windows thumbnail
// generation (as a handler for shell extension).
class QLGenHandler : public QObject {
    Q_OBJECT
public:
    QLGenHandler();
    ~QLGenHandler();
    void start();
    bool isFileCached(const QString &path);
private:
    bool lookUpFileInformation(const QString &path, QString *repo_id, QString *path_in_repo);
    SeafileRpcClient *rpc_client_;
};

#endif // SEADRIVE_CLIENT_QLGEN_HANDLER_H_
