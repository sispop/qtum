#include <qt/nfturllabel.h>
#include <qt/platformstyle.h>
#include <qt/styleSheet.h>

#include <QDesktopServices>
#include <QUrl>
#include <QIcon>

#define NFT_URI_ITEM_WIDTH 20
#define NFT_URI_ITEM_HEIGHT 30

NftUrlLabel::NftUrlLabel(QWidget* parent)
    : QLabel(parent)
{
    connect(this, &NftUrlLabel::clicked, this, &NftUrlLabel::on_clicked);
    QColor colorIcon = GetStringStyleValue("nfturllabel/color-icon", "#575757");
    m_pixmap = PlatformStyle::SingleColorIcon(QStringLiteral(":/icons/prompticon"), colorIcon).pixmap(NFT_URI_ITEM_WIDTH, NFT_URI_ITEM_HEIGHT);
    setPixmap(m_pixmap);
}

NftUrlLabel::~NftUrlLabel() {}

void NftUrlLabel::mouseReleaseEvent(QMouseEvent* event)
{
    if(event && event->button() == Qt::LeftButton)
        Q_EMIT clicked();
}

QString NftUrlLabel::getNftUrl() const
{
    return m_nftUrl;
}

void NftUrlLabel::setNftUrl(const QString &value)
{
    m_nftUrl = value;
}

void NftUrlLabel::on_clicked()
{
    QDesktopServices::openUrl(QUrl(m_nftUrl, QUrl::TolerantMode));
}