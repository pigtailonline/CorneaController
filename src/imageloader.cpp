#include "imageloader.h"

#include <QFileInfo>
#include <QImageReader>
#include <QDebug>
#include <QLinearGradient>
#include <QPainter>

ImageLoader::ImageLoader(QObject *parent)
    : QObject(parent)
{
}

ImageLoader::~ImageLoader()
{
    clearImages();
}

bool ImageLoader::loadImage(const QString &filePath)
{
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        emit errorOccurred(QString("File not found: %1").arg(filePath));
        return false;
    }

    QImage image(filePath);
    if (image.isNull()) {
        emit errorOccurred(QString("Failed to load image: %1").arg(filePath));
        return false;
    }

    QString name = fileInfo.fileName();

    // Handle duplicate names
    if (m_images.contains(name)) {
        int counter = 1;
        QString baseName = fileInfo.baseName();
        QString suffix = fileInfo.suffix();
        while (m_images.contains(name)) {
            name = QString("%1_%2.%3").arg(baseName).arg(counter++).arg(suffix);
        }
    }

    m_images[name] = image;

    // Select first loaded image
    if (m_currentImageName.isEmpty()) {
        m_currentImageName = name;
        emit imageSelected(name);
    }

    emit imageLoaded(name);
    return true;
}

bool ImageLoader::loadImages(const QStringList &filePaths)
{
    bool allSuccess = true;
    for (const QString &path : filePaths) {
        if (!loadImage(path)) {
            allSuccess = false;
        }
    }
    return allSuccess;
}

void ImageLoader::addImage(const QImage &image, const QString &name)
{
    if (image.isNull() || name.isEmpty()) {
        return;
    }

    bool isNew = !m_images.contains(name);

    // Replace if already exists (for test patterns)
    m_images[name] = image;

    // Emit imageLoaded only for new images
    if (isNew) {
        emit imageLoaded(name);
    }

    // Select this image
    m_currentImageName = name;
    emit imageSelected(name);
}

void ImageLoader::clearImages()
{
    m_images.clear();
    m_currentImageName.clear();
    emit imagesCleared();
}

QImage ImageLoader::getImage(const QString &name) const
{
    return m_images.value(name);
}

QImage ImageLoader::getCurrentImage() const
{
    return m_images.value(m_currentImageName);
}

QPixmap ImageLoader::getCurrentPixmap(const QSize &scaledSize) const
{
    QImage image = getCurrentImage();
    if (image.isNull()) {
        return QPixmap();
    }

    if (scaledSize.isValid() && !scaledSize.isEmpty()) {
        return QPixmap::fromImage(image.scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    return QPixmap::fromImage(image);
}

QString ImageLoader::getCurrentImageName() const
{
    return m_currentImageName;
}

QStringList ImageLoader::getImageNames() const
{
    return m_images.keys();
}

int ImageLoader::getImageCount() const
{
    return m_images.count();
}

bool ImageLoader::selectImage(const QString &name)
{
    if (!m_images.contains(name)) {
        return false;
    }

    m_currentImageName = name;
    emit imageSelected(name);
    return true;
}

bool ImageLoader::selectImageByIndex(int index)
{
    QStringList names = m_images.keys();
    if (index < 0 || index >= names.count()) {
        return false;
    }

    return selectImage(names[index]);
}

void ImageLoader::selectNextImage()
{
    if (m_images.isEmpty()) {
        return;
    }

    QStringList names = m_images.keys();
    int currentIndex = names.indexOf(m_currentImageName);
    int nextIndex = (currentIndex + 1) % names.count();
    selectImage(names[nextIndex]);
}

void ImageLoader::selectPreviousImage()
{
    if (m_images.isEmpty()) {
        return;
    }

    QStringList names = m_images.keys();
    int currentIndex = names.indexOf(m_currentImageName);
    int prevIndex = (currentIndex - 1 + names.count()) % names.count();
    selectImage(names[prevIndex]);
}

QSize ImageLoader::getImageSize(const QString &name) const
{
    QImage image = m_images.value(name);
    return image.isNull() ? QSize() : image.size();
}

QSize ImageLoader::getCurrentImageSize() const
{
    return getImageSize(m_currentImageName);
}

QImage ImageLoader::createSolidColor(int width, int height, const QColor &color)
{
    QImage image(width, height, QImage::Format_RGB888);
    image.fill(color);
    return image;
}

QImage ImageLoader::createGradient(int width, int height, const QColor &startColor, const QColor &endColor)
{
    QImage image(width, height, QImage::Format_RGB888);
    QPainter painter(&image);

    QLinearGradient gradient(0, 0, width, 0);
    gradient.setColorAt(0, startColor);
    gradient.setColorAt(1, endColor);

    painter.fillRect(image.rect(), gradient);
    return image;
}

QImage ImageLoader::createCheckerboard(int width, int height, int cellSize, const QColor &color1, const QColor &color2)
{
    QImage image(width, height, QImage::Format_RGB888);
    QPainter painter(&image);

    for (int y = 0; y < height; y += cellSize) {
        for (int x = 0; x < width; x += cellSize) {
            bool isEven = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            painter.fillRect(x, y, cellSize, cellSize, isEven ? color1 : color2);
        }
    }

    return image;
}

QImage ImageLoader::createColorBars(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    QPainter painter(&image);

    QList<QColor> colors = {
        Qt::white,
        Qt::yellow,
        Qt::cyan,
        Qt::green,
        Qt::magenta,
        Qt::red,
        Qt::blue,
        Qt::black
    };

    int barWidth = width / colors.count();
    for (int i = 0; i < colors.count(); ++i) {
        int x = i * barWidth;
        int w = (i == colors.count() - 1) ? (width - x) : barWidth;
        painter.fillRect(x, 0, w, height, colors[i]);
    }

    return image;
}

QStringList ImageLoader::getSupportedFormats()
{
    QStringList formats;
    for (const QByteArray &format : QImageReader::supportedImageFormats()) {
        formats.append(QString::fromLatin1(format));
    }
    return formats;
}

QString ImageLoader::getFileFilter()
{
    QStringList formats = getSupportedFormats();
    QStringList extensions;
    for (const QString &format : formats) {
        extensions.append(QString("*.%1").arg(format));
    }
    return QString("Images (%1)").arg(extensions.join(" "));
}
