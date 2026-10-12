#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <utility>

class QLocalServer;
class QLocalSocket;
class QTimer;
class QWidget;

// genko:// links from a web page (the style catalog's 「この絵柄を使う」; Python's genko/app/links.py): the system
// starts `genko app <link>`; when Genko is open already, that new start hands the link to the open one over a local
// socket and ends, so the link lands in the window the person is working in.
//
// register_links() tells the system that genko:// links open Genko (Windows: the user's registry; Linux: a desktop
// entry). A Mac learns it from the app's own Info.plist, so there is nothing to do from here.

namespace genko::app::links {

bool is_link(const QString& text);
// The local socket's name: genko-studio-links-<the user's name, letters and digits only>.
QString server_name();

// Hand the link to a Genko that is open already (false: none is).
bool send(const QString& link, int wait_ms = 800);

// Windows lets only the program the person just used bring a window to the front: this start passes that right on.
void allow_front();
// The window in front of the others (restored when minimized), before it asks something.
void come_forward(QWidget* window);

// Links handed over by later starts.
class Listener : public QObject {
    Q_OBJECT

public:
    explicit Listener(QObject* parent = nullptr);
    bool listening() const;

signals:
    void received(const QString& link);

private:
    void look();
    void take();
    void read(QLocalSocket* socket, bool final = false);

    QLocalServer* server_ = nullptr;
    QTimer* poll_ = nullptr;
};

// How the system should start Genko for a link: this build's genko and "app".
QStringList command();
// Make genko:// links open Genko for this user: (done, what happened in the person's words).
std::pair<bool, QString> register_links();
// `genko register-links`: {"ok", "message"} on stdout; 0 when done, else 1.
int register_command(int argc, char** argv);

}  // namespace genko::app::links
