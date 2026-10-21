#pragma once
#include <QCollator>
#include <QTreeWidget>

class DirectoryItem : public QTreeWidgetItem {
  public:
    explicit DirectoryItem(QTreeWidget *tree) : QTreeWidgetItem(tree) {}
    bool operator<(const QTreeWidgetItem &other) const override {
        int column = treeWidget()->sortColumn();
        if (column == 1 || column == 2) {
            auto left = data(column, Qt::UserRole).toULongLong();
            auto right = other.data(column, Qt::UserRole).toULongLong();
            if (left != right)
                return left < right;
        }
        static const QCollator collator = [] {
            QCollator c;
            c.setNumericMode(true);
            c.setCaseSensitivity(Qt::CaseInsensitive);
            return c;
        }();
        int comparison = collator.compare(text(0), other.text(0));
        return comparison ? comparison < 0 : text(0) < other.text(0);
    }
};
