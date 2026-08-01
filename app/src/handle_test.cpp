// Standalone functional check for KumDbHandle, independent of any widgets --
// exercises exactly the code that's trickiest to get right (C/C++ memory
// lifetimes across the kumdb.h boundary). Built as its own small target
// (kumdb_studio_handle_test) since it only needs QtCore, not QtWidgets.
#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>

#include "KumDbHandle.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    system("rm -rf /tmp/kumdb_handle_test");

    KumDbHandle db;
    QString err;

    CHECK(db.open("/tmp/kumdb_handle_test", &err));
    CHECK(db.isOpen());

    QVector<KColumnMeta> cols;
    cols.append({"name", KDB_TYPE_STRING, false, false});
    cols.append({"age", KDB_TYPE_INT, true, true});
    cols.append({"gpa", KDB_TYPE_FLOAT, true, false});
    CHECK(db.createTable("students", cols, &err));
    CHECK(db.tableExists("students"));

    QVector<KColumnMeta> readBack = db.schema("students");
    CHECK(readBack.size() == 3);
    if (readBack.size() == 3) {
        CHECK(readBack[0].name == "name");
        CHECK(readBack[1].indexed == true);
    }

    QVector<QPair<QString, QVariant>> row1 = {{"name", QVariant("Alice")}, {"age", QVariant::fromValue<qint64>(20)}, {"gpa", QVariant(3.9)}};
    QVector<QPair<QString, QVariant>> row2 = {{"name", QVariant("Bob")}, {"age", QVariant::fromValue<qint64>(22)}, {"gpa", QVariant(3.1)}};
    CHECK(db.addRow("students", row1, &err));
    CHECK(db.addRow("students", row2, &err));

    QVector<KRow> all = db.find("students", {}, &err);
    CHECK(all.size() == 2);

    QVector<KRow> filtered = db.find("students", {"age__gt=20"}, &err);
    CHECK(filtered.size() == 1);
    if (filtered.size() == 1) {
        bool foundName = false;
        for (auto &f : filtered[0].fields)
            if (f.first == "name") { CHECK(f.second.toString() == "Bob"); foundName = true; }
        CHECK(foundName);
    }

    // id=1 regression check -- the exact case that broke before the fix
    QVector<KRow> byId = db.find("students", {"id=1"}, &err);
    CHECK(byId.size() == 1);

    quint64 aliceId = 0;
    for (auto &r : all) {
        for (auto &f : r.fields)
            if (f.first == "name" && f.second.toString() == "Alice") aliceId = r.id;
    }
    CHECK(aliceId != 0);

    CHECK(db.updateCell("students", aliceId, "gpa", QVariant(4.0), &err));
    QVector<KRow> afterUpdate = db.find("students", {"id=" + QString::number(aliceId)}, &err);
    CHECK(afterUpdate.size() == 1);
    if (afterUpdate.size() == 1) {
        bool sawGpa = false;
        for (auto &f : afterUpdate[0].fields)
            if (f.first == "gpa") { CHECK(f.second.toDouble() > 3.99); sawGpa = true; }
        CHECK(sawGpa);
    }

    KumDbHandle::SqlResult sr = db.execSql("SELECT * FROM students WHERE age >= 20 ORDER BY age DESC");
    CHECK(sr.ok);
    CHECK(sr.hasRows);
    CHECK(sr.rows.size() == 2);

    KumDbHandle::SqlResult sr2 = db.execSql("UPDATE students SET gpa = 2.0 WHERE name = 'Bob'");
    CHECK(sr2.ok);
    CHECK(!sr2.hasRows);
    CHECK(sr2.affected == 1);

    CHECK(db.deleteRow("students", aliceId, &err));
    QVector<KRow> afterDelete = db.find("students", {}, &err);
    CHECK(afterDelete.size() == 1);

    CHECK(db.dropTable("students", &err));
    CHECK(!db.tableExists("students"));

    db.close();
    CHECK(!db.isOpen());

    system("rm -rf /tmp/kumdb_handle_test");

    printf(failures == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
