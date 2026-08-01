#include "NewTableDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>

static const char *kTypeNames[] = {"TEXT", "INT", "FLOAT", "BOOL", "BLOB"};
static const KdbFieldType kTypeValues[] = {KDB_TYPE_STRING, KDB_TYPE_INT, KDB_TYPE_FLOAT, KDB_TYPE_BOOL, KDB_TYPE_BLOB};
static const int kTypeCount = 5;

enum { ColName = 0, ColType = 1, ColNotNull = 2, ColIndexed = 3, ColCount = 4 };

NewTableDialog::NewTableDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("New Table");
    resize(520, 360);

    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText("table name");
    form->addRow("Table name:", m_nameEdit);
    layout->addLayout(form);

    m_colTable = new QTableWidget(0, ColCount, this);
    m_colTable->setHorizontalHeaderLabels({"Column", "Type", "NOT NULL", "Indexed"});
    m_colTable->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_colTable->horizontalHeader()->setSectionResizeMode(ColType, QHeaderView::ResizeToContents);
    m_colTable->horizontalHeader()->setSectionResizeMode(ColNotNull, QHeaderView::ResizeToContents);
    m_colTable->horizontalHeader()->setSectionResizeMode(ColIndexed, QHeaderView::ResizeToContents);
    layout->addWidget(m_colTable);

    auto *rowButtons = new QHBoxLayout;
    auto *addBtn = new QPushButton("+ Add column", this);
    auto *removeBtn = new QPushButton("- Remove column", this);
    rowButtons->addWidget(addBtn);
    rowButtons->addWidget(removeBtn);
    rowButtons->addStretch();
    layout->addLayout(rowButtons);

    connect(addBtn, &QPushButton::clicked, this, &NewTableDialog::addColumnRow);
    connect(removeBtn, &QPushButton::clicked, this, &NewTableDialog::removeSelectedColumnRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "New Table", "Table needs a name.");
            return;
        }
        if (columns().isEmpty()) {
            QMessageBox::warning(this, "New Table", "Add at least one column (id/created_at/updated_at are automatic, don't add those).");
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &NewTableDialog::reject);
    layout->addWidget(buttons);

    addColumnRow();
}

void NewTableDialog::addColumnRow() {
    int row = m_colTable->rowCount();
    m_colTable->insertRow(row);
    m_colTable->setItem(row, ColName, new QTableWidgetItem(""));

    auto *typeBox = new QComboBox(m_colTable);
    for (int i = 0; i < kTypeCount; i++) typeBox->addItem(kTypeNames[i]);
    m_colTable->setCellWidget(row, ColType, typeBox);

    auto *notNullBox = new QCheckBox(m_colTable);
    auto *notNullWrap = new QWidget(m_colTable);
    auto *notNullLayout = new QHBoxLayout(notNullWrap);
    notNullLayout->addWidget(notNullBox);
    notNullLayout->setAlignment(Qt::AlignCenter);
    notNullLayout->setContentsMargins(0, 0, 0, 0);
    m_colTable->setCellWidget(row, ColNotNull, notNullWrap);

    auto *indexedBox = new QCheckBox(m_colTable);
    auto *indexedWrap = new QWidget(m_colTable);
    auto *indexedLayout = new QHBoxLayout(indexedWrap);
    indexedLayout->addWidget(indexedBox);
    indexedLayout->setAlignment(Qt::AlignCenter);
    indexedLayout->setContentsMargins(0, 0, 0, 0);
    m_colTable->setCellWidget(row, ColIndexed, indexedWrap);
}

void NewTableDialog::removeSelectedColumnRow() {
    int row = m_colTable->currentRow();
    if (row >= 0) m_colTable->removeRow(row);
}

QString NewTableDialog::tableName() const {
    return m_nameEdit->text().trimmed();
}

QVector<KColumnMeta> NewTableDialog::columns() const {
    QVector<KColumnMeta> out;
    for (int row = 0; row < m_colTable->rowCount(); row++) {
        auto *nameItem = m_colTable->item(row, ColName);
        QString name = nameItem ? nameItem->text().trimmed() : QString();
        if (name.isEmpty()) continue;

        auto *typeBox = qobject_cast<QComboBox *>(m_colTable->cellWidget(row, ColType));
        auto *notNullWrap = m_colTable->cellWidget(row, ColNotNull);
        auto *indexedWrap = m_colTable->cellWidget(row, ColIndexed);
        auto *notNullBox = notNullWrap ? notNullWrap->findChild<QCheckBox *>() : nullptr;
        auto *indexedBox = indexedWrap ? indexedWrap->findChild<QCheckBox *>() : nullptr;

        KColumnMeta c;
        c.name = name;
        c.type = typeBox ? kTypeValues[typeBox->currentIndex()] : KDB_TYPE_STRING;
        c.nullable = notNullBox ? !notNullBox->isChecked() : true;
        c.indexed = indexedBox ? indexedBox->isChecked() : false;
        out.append(c);
    }
    return out;
}
