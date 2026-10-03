#pragma once

#include <QAbstractListModel>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace rostrum::app {

// A list model over QVariantMap rows with a stable "key" field. When the keys are unchanged only
// the changed rows are updated, so delegates (and a slider being dragged in one) survive.
// Roles: "row" (the whole map) and "key".
class RowsModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { RowRole = Qt::UserRole + 1, KeyRole };

    explicit RowsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_rows.size()); }
    void setRows(const QVariantList &rows);

Q_SIGNALS:
    void countChanged();

private:
    QVariantList m_rows;
};

} // namespace rostrum::app
