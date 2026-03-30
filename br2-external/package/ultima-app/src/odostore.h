#ifndef ODOSTORE_H
#define ODOSTORE_H

#include <QObject>
#include <QString>

class OdoStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double totalOdo READ totalOdo WRITE setTotalOdo NOTIFY totalOdoChanged)
    Q_PROPERTY(double tripOdo READ tripOdo WRITE setTripOdo NOTIFY tripOdoChanged)

public:
    explicit OdoStore(const QString &path, QObject *parent = nullptr);

    double totalOdo() const { return m_totalOdo; }
    double tripOdo() const { return m_tripOdo; }
    void setTotalOdo(double v);
    void setTripOdo(double v);

public slots:
    void save();

signals:
    void totalOdoChanged();
    void tripOdoChanged();

private:
    void load();
    QString m_path;
    double m_totalOdo;
    double m_tripOdo;
};

#endif
