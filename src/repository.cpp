/*
 *  Copyright (C) 2007  Thiago Macieira <thiago@kde.org>
 *  Copyright (C) 2009 Thomas Zander <zander@kde.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "repository.h"
#include "CommandLineParser.h"
#include <QTextStream>
#include <QDebug>
#include <QDir>
#include <QLinkedList>

static const int maxSimultaneousProcesses = 100;

class ProcessCache: QLinkedList<Repository *>
{
public:
    void touch(Repository *repo)
    {
        remove(repo);

        // if the cache is too big, remove from the front
        while (size() >= maxSimultaneousProcesses)
            takeFirst()->closeFastImport();

        // append to the end
        append(repo);
    }

    inline void remove(Repository *repo)
    {
#if QT_VERSION >= 0x040400
        removeOne(repo);
#else
        removeAll(repo);
#endif
    }
};
static ProcessCache processCache;

Repository::Repository(const Rules::Repository &rule)
  : name(rule.name), commitCount(0), outstandingTransactions(0),  lastmark(0), processHasStarted(false)
{
    foreach (Rules::Repository::Branch branchRule, rule.branches) {
        Branch branch;
        branch.created = 0;     // not created

        branches.insert(branchRule.name, branch);
    }

    // create the default branch
    branches["master"].created = 1;

    fastImport.setWorkingDirectory(name);
    if (!CommandLineParser::instance()->contains("dry-run")) {
        if (!QDir(name).exists()) { // repo doesn't exist yet.
            qDebug() << "Creating new repository" << name;
            QDir::current().mkpath(name);
            QProcess init;
            init.setWorkingDirectory(name);
            init.start("git", QStringList() << "--bare" << "init");
            init.waitForFinished(-1);
	    QDir::current().mkpath(name + "/info/fast-import");
	    {
	        QFile marks(name + "/info/fast-import/marks");
	        marks.open(QIODevice::WriteOnly);
	        marks.close();
	    }
        }
    }
}

Repository::~Repository()
{
    Q_ASSERT(outstandingTransactions == 0);
    closeFastImport();
}

void Repository::closeFastImport()
{
    if (fastImport.state() != QProcess::NotRunning) {
        fastImport.write("checkpoint\n");
        fastImport.waitForBytesWritten(-1);
        fastImport.closeWriteChannel();
        if (!fastImport.waitForFinished()) {
            fastImport.terminate();
            if (!fastImport.waitForFinished(200))
                qWarning() << "git-fast-import for repository" << name << "did not die";
        }
    }
    processHasStarted = false;
    processCache.remove(this);
}

void Repository::reloadBranches()
{
    QProcess revParse;
    revParse.setWorkingDirectory(name);
    revParse.start("git", QStringList() << "rev-parse" << "--symbolic" << "--branches");
    revParse.waitForFinished(-1);

    if (revParse.exitCode() == 0 && revParse.bytesAvailable()) {
        while (revParse.canReadLine()) {
            QByteArray branchName = revParse.readLine().trimmed();

            //qDebug() << "Repo" << name << "reloaded branch" << branchName;

	    Q_ASSERT(branches[branchName].created);

            fastImport.write("reset refs/heads/" + branchName +
                             "\nfrom refs/heads/" + branchName + "^0\n\n"
                             "progress Branch refs/heads/" + branchName + " reloaded\n");
        }
    }
}

void Repository::createBranch(const QString &branch, int revnum,
                              const QString &branchFrom, int branchRevNum)
{
    startFastImport();
    if (!branches.contains(branch)) {
        qWarning() << branch << "is not a known branch in repository" << name << endl
                   << "Going to create it automatically";
    }

    QByteArray branchRef = branch.toUtf8();
        if (!branchRef.startsWith("refs/"))
            branchRef.prepend("refs/heads/");

    Branch &br = branches[branch];
    if (br.created && br.created != revnum) {
        QByteArray backupBranch = branchRef + '_' + QByteArray::number(revnum);
        qWarning() << branch << "already exists; backing up to" << backupBranch;

        fastImport.write("reset " + backupBranch + "\nfrom " + branchRef + "\n\n");
    }

    Branch &brFrom = branches[branchFrom];
    if (!brFrom.created) {
        qCritical() << branch << "in repository" << name
                    << "is branching from branch" << branchFrom
                    << "but the latter doesn't exist. Can't continue.";
        exit(1);
    }

    // now create the branch
    br.created = revnum;
    br.commits.append(revnum);

    QByteArray branchFromRef, branchFromDesc;

    int closestCommit = branchRevNum;
    if (branchRevNum == brFrom.commits.last()) {
        branchFromDesc = "from branch " + branchFrom.toUtf8();
    } else {
        QVector<int>::const_iterator it = qUpperBound(brFrom.commits, branchRevNum);
        closestCommit = it == brFrom.commits.begin() ? branchRevNum : *--it;
	branchFromDesc = "from branch " + branchFrom.toUtf8() + " at r" + QByteArray::number(branchRevNum);
	if (closestCommit != branchRevNum)
	    branchFromDesc += " => r" + QByteArray::number(closestCommit);
    }

    if(commitMarks.contains(closestCommit)) {
        int mark = commitMarks[closestCommit];
        branchFromRef = ":" + QByteArray::number(mark);
	commitMarks[revnum] = mark;
    } else {
        qWarning() << branch << "in repository" << name << "is branching but no exported commits exist in repository"
                << "creating an empty branch.";
        branchFromRef = branchFrom.toUtf8();
        if (!branchFromRef.startsWith("refs/"))
            branchFromRef.prepend("refs/heads/");
	branchFromDesc = "at unknown r" + QByteArray::number(branchRevNum);
    }

    fastImport.write("reset " + branchRef + "\nfrom " + branchFromRef + "\n\n"
		     "progress SVN r" + QByteArray::number(revnum)
		     + " branch " + branch.toUtf8() + " = " + branchFromRef
		     + " # " + branchFromDesc
		     + "\n\n");
}

Repository::Transaction *Repository::newTransaction(const QString &branch, const QString &svnprefix,
                                                    int revnum)
{
    startFastImport();
    if (!branches.contains(branch)) {
        qWarning() << branch << "is not a known branch in repository" << name << endl
                   << "Going to create it automatically";
    }

    Transaction *txn = new Transaction;
    txn->repository = this;
    txn->branch = branch.toUtf8();
    txn->svnprefix = svnprefix.toUtf8();
    txn->datetime = 0;
    txn->revnum = revnum;

    if ((++commitCount % CommandLineParser::instance()->optionArgument(QLatin1String("commit-interval"), QLatin1String("10000")).toInt()) == 0)
        // write everything to disk every 10000 commits
        fastImport.write("checkpoint\n");
    outstandingTransactions++;
    return txn;
}

void Repository::createAnnotatedTag(const QString &ref, const QString &svnprefix,
                                    int revnum,
                                    const QByteArray &author, uint dt,
                                    const QByteArray &log)
{
    QString tagName = ref;
    if (tagName.startsWith("refs/tags/"))
        tagName.remove(0, 10);

    if (!annotatedTags.contains(tagName))
        printf("Creating annotated tag %s (%s)\n", qPrintable(tagName), qPrintable(ref));
    else
        printf("Re-creating annotated tag %s\n", qPrintable(tagName));

    AnnotatedTag &tag = annotatedTags[tagName];
    tag.supportingRef = ref;
    tag.svnprefix = svnprefix.toUtf8();
    tag.revnum = revnum;
    tag.author = author;
    tag.log = log;
    tag.dt = dt;
}

void Repository::finalizeTags()
{
    if (annotatedTags.isEmpty())
        return;

    printf("Finalising tags for %s...", qPrintable(name));
    startFastImport();

    QHash<QString, AnnotatedTag>::ConstIterator it = annotatedTags.constBegin();
    for ( ; it != annotatedTags.constEnd(); ++it) {
        const QString &tagName = it.key();
        const AnnotatedTag &tag = it.value();

        QByteArray message = tag.log;
        if (!message.endsWith('\n'))
            message += '\n';
        if (CommandLineParser::instance()->contains("add-metadata"))
            message += "\nsvn path=" + tag.svnprefix + "; revision=" + QByteArray::number(tag.revnum) + "\n";

        {
            QByteArray branchRef = tag.supportingRef.toUtf8();
            if (!branchRef.startsWith("refs/"))
                branchRef.prepend("refs/heads/");

            QTextStream s(&fastImport);
            s << "progress Creating annotated tag " << tagName << " from ref " << branchRef << endl
              << "tag " << tagName << endl
              << "from " << branchRef << endl
              << "tagger " << QString::fromUtf8(tag.author) << ' ' << tag.dt << " -0000" << endl
              << "data " << message.length() << endl;
        }

        fastImport.write(message);
        fastImport.putChar('\n');
        if (!fastImport.waitForBytesWritten(-1))
            qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));

        printf(" %s", qPrintable(tagName));
        fflush(stdout);
    }

    while (fastImport.bytesToWrite())
        if (!fastImport.waitForBytesWritten(-1))
            qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));
    printf("\n");
}

void Repository::startFastImport()
{
    if (fastImport.state() == QProcess::NotRunning) {
        if (processHasStarted)
            qFatal("git-fast-import has been started once and crashed?");
        processHasStarted = true;

        // start the process
        QString outputFile = name;
        outputFile.replace('/', '_');
        outputFile.prepend("log-");
        fastImport.setStandardOutputFile(outputFile, QIODevice::Append);
        fastImport.setProcessChannelMode(QProcess::MergedChannels);

        if (!CommandLineParser::instance()->contains("dry-run")) {
	    fastImport.start("git", QStringList() << "fast-import" << "--relative-marks" << "--import-marks=marks" << "--export-marks=marks" << "--force");
        } else {
            fastImport.start("/bin/cat", QStringList());
        }

        reloadBranches();
    }
}

Repository::Transaction::~Transaction()
{
    --repository->outstandingTransactions;
}

void Repository::Transaction::setAuthor(const QByteArray &a)
{
    author = a;
}

void Repository::Transaction::setDateTime(uint dt)
{
    datetime = dt;
}

void Repository::Transaction::setLog(const QByteArray &l)
{
    log = l;
}

void Repository::Transaction::noteCopyFromBranch (const QString &branchFrom, int branchRevNum)
{
    Branch &brFrom = repository->branches[branchFrom];
    if (!brFrom.created) {
        qWarning() << branch << "is copying from branch" << branchFrom
                    << "but the latter doesn't exist.  Continuing, assuming the files exist.";
	return;
    }

    int closestCommit = branchRevNum;
    if (branchRevNum != brFrom.commits.last()) {
        QVector<int>::const_iterator it = qUpperBound(brFrom.commits, branchRevNum);
        closestCommit = it == brFrom.commits.begin() ? branchRevNum : *--it;
    }

    if (!repository->commitMarks.contains(closestCommit)) {
	qWarning() << "Unknown revision r" << QByteArray::number(closestCommit)
		   << ".  Continuing, assuming the files exist.";
	return;
    }

    qWarning() << "repository " + repository->name + " branch " + branch + " has some files copied from " + branchFrom + "@" + QByteArray::number(branchRevNum);

    int mark = repository->commitMarks[closestCommit];
    if (!merges.contains(mark))
	merges.append(mark);
}

void Repository::Transaction::deleteFile(const QString &path)
{
    QString pathNoSlash = path;
    if(pathNoSlash.endsWith('/'))
        pathNoSlash.chop(1);
    deletedFiles.append(pathNoSlash);
}

QIODevice *Repository::Transaction::addFile(const QString &path, int mode, qint64 length)
{
    int mark = ++repository->lastmark;

    if (modifiedFiles.capacity() == 0)
        modifiedFiles.reserve(2048);
    modifiedFiles.append("M ");
    modifiedFiles.append(QByteArray::number(mode, 8));
    modifiedFiles.append(" :");
    modifiedFiles.append(QByteArray::number(mark));
    modifiedFiles.append(' ');
    modifiedFiles.append(path.toUtf8());
    modifiedFiles.append("\n");

    if (!CommandLineParser::instance()->contains("dry-run")) {
        repository->fastImport.write("blob\nmark :");
        repository->fastImport.write(QByteArray::number(mark));
        repository->fastImport.write("\ndata ");
        repository->fastImport.write(QByteArray::number(length));
        repository->fastImport.write("\n", 1);
    }

    return &repository->fastImport;
}

void Repository::Transaction::commit()
{
    processCache.touch(repository);

    int mark = ++repository->lastmark;

    // create the commit message
    QByteArray message = log;
    if (!message.endsWith('\n'))
        message += '\n';
    if (CommandLineParser::instance()->contains("add-metadata"))
        message += "\nsvn path=" + svnprefix + "; revision=" + QByteArray::number(revnum) + "\n";

    int parentmark = 0;
    Branch &br = repository->branches[branch];
    if (br.created) {
	parentmark = repository->commitMarks[br.commits.last()];
    } else {
	qWarning() << "Branch" << branch << "in repository" << repository->name << "doesn't exist at revision"
		   << revnum << "-- did you resume from the wrong revision?";
	br.created = revnum;
    }
    br.commits.append(revnum);

    {
        QByteArray branchRef = branch;
        if (!branchRef.startsWith("refs/"))
            branchRef.prepend("refs/heads/");

        QTextStream s(&repository->fastImport);
        s << "commit " << branchRef << endl;
        s << "mark :" << QByteArray::number(mark) << endl;
        repository->commitMarks.insert(revnum, mark);
        s << "committer " << QString::fromUtf8(author) << ' ' << datetime << " -0000" << endl;

        s << "data " << message.length() << endl;
    }

    repository->fastImport.write(message);
    repository->fastImport.putChar('\n');

    // note some of the inferred merges
    QByteArray desc = "";
    int i = 0;
    foreach (int merge, merges) {
	if (merge == parentmark)
	    continue;

	// FIXME: options:
	//   (1) ignore the 15 merges limit
	//   (2) don't emit more than 15 merges
	//   (3) create another commit on branch to soak up additional parents
	// we've chosen option (2) for now, since only artificial commits
	// created by cvs2svn seem to have this issue
	if (++i >= 16) {
	    qWarning() << "too many merge parents";
	    break;
	}

	QByteArray m = " :" + QByteArray::number(merge);
	desc += m;
	repository->fastImport.write("merge" + m + "\n");
    }

    // write the file deletions
    if (deletedFiles.contains(""))
        repository->fastImport.write("deleteall\n");
    else
        foreach (QString df, deletedFiles)
            repository->fastImport.write("D " + df.toUtf8() + "\n");

    // write the file modifications
    repository->fastImport.write(modifiedFiles);

    repository->fastImport.write("\nprogress SVN r" + QByteArray::number(revnum)
                                 + " branch " + branch + " = :" + QByteArray::number(mark)
				 + (desc.isEmpty() ? "" : " # merge from") + desc
				 + "\n\n");
    printf(" %d modifications from SVN %s to %s/%s",
           deletedFiles.count() + modifiedFiles.count(), svnprefix.data(),
           qPrintable(repository->name), branch.data());

    while (repository->fastImport.bytesToWrite())
        if (!repository->fastImport.waitForBytesWritten(-1))
            qFatal("Failed to write to process: %s", qPrintable(repository->fastImport.errorString()));
}
