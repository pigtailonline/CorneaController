#ifndef IMAGELOADER_H
#define IMAGELOADER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QImage>
#include <QPixmap>
#include <QMap>

class ImageLoader : public QObject
{
    Q_OBJECT

public:
    explicit ImageLoader(QObject *parent = nullptr);
    ~ImageLoader();

    // Image loading
    bool loadImage(const QString &filePath);
    bool loadImages(const QStringList &filePaths);
    void addImage(const QImage &image, const QString &name);
    void clearImages();

    // Image access
    QImage getImage(const QString &name) const;
    QImage getCurrentImage() const;
    QPixmap getCurrentPixmap(const QSize &scaledSize = QSize()) const;
    QString getCurrentImageName() const;

    // Image list management
    QStringList getImageNames() const;
    int getImageCount() const;
    bool hasImages() const { return !m_images.isEmpty(); }

    // Navigation
    bool selectImage(const QString &name);
    bool selectImageByIndex(int index);
    void selectNextImage();
    void selectPreviousImage();

    // Image info
    QSize getImageSize(const QString &name) const;
    QSize getCurrentImageSize() const;

    // Test patterns
    QImage createSolidColor(int width, int height, const QColor &color);
    QImage createGradient(int width, int height, const QColor &startColor, const QColor &endColor);
    QImage createCheckerboard(int width, int height, int cellSize, const QColor &color1, const QColor &color2);
    QImage createColorBars(int width, int height);

    // Supported formats
    static QStringList getSupportedFormats();
    static QString getFileFilter();

signals:
    void imageLoaded(const QString &name);
    void imageSelected(const QString &name);
    void imagesCleared();
    void errorOccurred(const QString &error);

private:
    QMap<QString, QImage> m_images;
    QString m_currentImageName;
};

#endif // IMAGELOADER_H
