#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QMainWindow>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMap>
#include <QString>
#include <QListWidget>
#include <QStackedWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QTimer>
#include <QSettings>
#include <QProcess>
#include <QStandardPaths>
#include <QResizeEvent>
#include <QWidget>

class LoginWindow : public QMainWindow
{
    Q_OBJECT

public:
    LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onLoginClicked();
    void onLoginReply(QNetworkReply *reply);
    void onLoginError(QNetworkReply::NetworkError error);
    void onLanguageButtonClicked();
    void onLanguageSelected(QAction *action);
    void onHealthCheckReply(QNetworkReply *reply, const QString &server, const QString &username, const QString &password);
    void onBackClicked();
    void onRefreshClicked();
    void onChangePasswordClicked();
    void onChangePasswordReply(QNetworkReply *reply);
    void onVmListReply(QNetworkReply *reply);
    void onVmStartClicked(const QString &vmId);
    void onVmStopClicked(const QString &vmId);
    void onVmRestartClicked(const QString &vmId);
    void onVmRestoreClicked(const QString &vmId);
    void onVmConnectClicked(const QString &vmId);
    void onVmRdpFileReply(QNetworkReply *reply, const QString &vmId);
    void onVmLoginReply(QNetworkReply *reply, const QString &vmId);
    void onVmStatusReply(QNetworkReply *reply, const QString &vmId);
    void onVmStartReply(QNetworkReply *reply, const QString &vmId);
    void onVmStopReply(QNetworkReply *reply, const QString &vmId);
    void onQVmRestartReply(QNetworkReply *reply, const QString &vmId);
    void onVmRestoreReply(QNetworkReply *reply, const QString &vmId);
    void onAutoLoginChanged(Qt::CheckState state);
    void fetchVmSnapshot(const QString &vmId);
    void onVmSnapshotReply(QNetworkReply *reply, const QString &vmId);
    void onRdpProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onRdpProcessErrorOccurred(QProcess::ProcessError error);

private:
    void setupUi();
    void setupLoginUi(QWidget *widget);
    void setupVmListUi(QWidget *widget);
    void sendLoginRequest(const QString &server, const QString &username, const QString &password);
    void checkServerHealth(const QString &server, const QString &username, const QString &password);
    void fetchVmList();
    void fetchVmStatus(const QString &vmId);
    void initTranslations();
    void updateLanguage(const QString &languageCode);
    QString translate(const QString &key);
    void updateVmListWidget();
    QFrame* createVmItemWidget(const QString &vmName, const QString &vmId, const QString &status);
    void updateVmStatus(const QString &vmId, const QString &status);
    QString buildApiUrl(const QString &path);
    void loadSettings();
    void saveSettings();
    bool isTokenExpired(QNetworkReply *reply);
    void handleTokenExpired();
    bool validateAndNormalizeServer(QString &server);
    void startHeartbeat();
    void stopHeartbeat();
    void sendHeartbeat();
    void onHeartbeatReply(QNetworkReply *reply);

    QLineEdit *m_serverEdit;
    QLineEdit *m_usernameEdit;
    QLineEdit *m_passwordEdit;
    QCheckBox *m_rememberPasswordCheckBox;
    QCheckBox *m_autoLoginCheckBox;
    QPushButton *m_languageButton;
    QMenu *m_languageMenu;
    QPushButton *m_loginButton;
    QLabel *m_statusLabel;
    QNetworkAccessManager *m_networkManager;
    QLabel *m_titleLabel;
    QByteArray m_healthCheckData;
    QByteArray m_vmListData;

    QWidget *m_vmListWidget;
    QWidget *m_vmListContainer;
    QPushButton *m_backButton;
    QPushButton *m_refreshButton;
    QPushButton *m_changePasswordButton;
    QLabel *m_vmTitleLabel;
    QLabel *m_vmStatusLabel;
    QStackedWidget *m_stackedWidget;

    QString m_server;
    QString m_token;
    QString m_username;
    QString m_currentLanguage;
    QMap<QString, QMap<QString, QString>> m_translations;
    QMap<QString, QLabel*> m_vmStatusLabels;
    QMap<QString, QByteArray> m_vmStatusData;
    QMap<QString, QPushButton*> m_vmStartButtons;
    QMap<QString, QByteArray> m_vmStartData;
    QMap<QString, QTimer*> m_vmStartTimers;
    QMap<QString, QPushButton*> m_vmConnectButtons;
    QMap<QString, QPushButton*> m_vmRestoreButtons;
    QMap<QString, bool> m_vmHasSnapshot;
    QProcess *m_rdpProcess;
    QTimer *m_heartbeatTimer;
};

#endif // LOGINWINDOW_H