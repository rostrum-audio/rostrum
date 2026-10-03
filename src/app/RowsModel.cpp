#include "app/RowsModel.h"

namespace rostrum::app {

namespace {
QString keyOf(const QVariant &row)
{
    return row.toMap().value(QStringLiteral("key")).toString();
}
} // namespace

RowsModel::RowsModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int RowsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant RowsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return {};
    }
    const QVariant &row = m_rows.at(index.row());
    return role == KeyRole ? QVariant(keyOf(row)) : role == RowRole ? row : QVariant();
}

QHash<int, QByteArray> RowsModel::roleNames() const
{
    return {{RowRole, "row"}, {KeyRole, "key"}};
}

void RowsModel::setRows(const QVariantList &rows)
{
    bool sameKeys = rows.size() == m_rows.size();
    for (qsizetype i = 0; sameKeys && i < rows.size(); ++i) {
        sameKeys = keyOf(rows.at(i)) == keyOf(m_rows.at(i));
    }
    if (!sameKeys) {
        const bool countChange = rows.size() != m_rows.size();
        beginResetModel();
        m_rows = rows;
        endResetModel();
        if (countChange) {
            Q_EMIT countChanged();
        }
        return;
    }
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (rows.at(i) != m_rows.at(i)) {
            m_rows[i] = rows.at(i);
            Q_EMIT dataChanged(index(int(i)), index(int(i)), {RowRole});
        }
    }
}

} // namespace rostrum::app
