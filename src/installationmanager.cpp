/*
Copyright (C) 2012 Sebastian Herbord. All rights reserved.

This file is part of Mod Organizer.

Mod Organizer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Mod Organizer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Mod Organizer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "installationmanager.h"
#include "utility.h"
#include "report.h"
#include "categories.h"
#include "questionboxmemory.h"
#include "settings.h"
#include "queryoverwritedialog.h"
#include "messagedialog.h"
#include "iplugininstallersimple.h"
#include "iplugininstallercustom.h"
#include "nexusinterface.h"
#include "selectiondialog.h"
#include <scopeguard.h>
#include <installationtester.h>
#include <gameinfo.h>
#include <utility.h>
#include <QFileInfo>
#include <QLibrary>
#include <QInputDialog>
#include <QRegExp>
#include <QDir>
#include <QMessageBox>
#include <QSettings>
#include <Shellapi.h>
#include <QPushButton>
#include <QApplication>
#include <QDateTime>
#include <QDirIterator>
#include <boost/assign.hpp>


using namespace MOBase;
using namespace MOShared;


typedef Archive* (*CreateArchiveType)();



template <typename T> T resolveFunction(QLibrary &lib, const char *name)
{
  T temp = reinterpret_cast<T>(lib.resolve(name));
  if (temp == NULL) {
    throw std::runtime_error(QObject::tr("invalid 7-zip32.dll: %1").arg(lib.errorString()).toLatin1().constData());
  }
  return temp;
}


InstallationManager::InstallationManager(QWidget *parent)
  : QObject(parent), m_ParentWidget(parent),
    m_InstallationProgress(parent), m_SupportedExtensions(boost::assign::list_of("zip")("rar")("7z")("fomod"))
{
  QLibrary archiveLib("dlls\\archive.dll");
  if (!archiveLib.load()) {
    throw MyException(tr("archive.dll not loaded: \"%1\"").arg(archiveLib.errorString()));
  }

  CreateArchiveType CreateArchiveFunc = resolveFunction<CreateArchiveType>(archiveLib, "CreateArchive");

  m_CurrentArchive = CreateArchiveFunc();
  if (!m_CurrentArchive->isValid()) {
    throw MyException(getErrorString(m_CurrentArchive->getLastError()));
  }

  m_InstallationProgress.setWindowFlags(m_InstallationProgress.windowFlags() & (~Qt::WindowContextHelpButtonHint));
}


InstallationManager::~InstallationManager()
{
  delete m_CurrentArchive;
}


void InstallationManager::queryPassword(LPSTR password)
{
  QString result = QInputDialog::getText(NULL, tr("Password required"), tr("Password"), QLineEdit::Password);
  strncpy(password, result.toLocal8Bit().constData(), MAX_PASSWORD_LENGTH);
}


void InstallationManager::mapToArchive(const DirectoryTree::Node *node, std::wstring path, FileData * const *data)
{
  if (path.length() > 0) {
    path.append(L"\\");
  }

  for (DirectoryTree::const_leaf_iterator iter = node->leafsBegin(); iter != node->leafsEnd(); ++iter) {
    data[iter->getIndex()]->setSkip(false);
    std::wstring temp = path.substr().append(ToWString(iter->getName()));
    data[iter->getIndex()]->setOutputFileName(temp.c_str());
  }

  for (DirectoryTree::const_node_iterator iter = node->nodesBegin(); iter != node->nodesEnd(); ++iter) {
    if ((*iter)->getData().index != -1) {
      data[(*iter)->getData().index]->setSkip(false);
      data[(*iter)->getData().index]->setOutputFileName(path.substr().append(ToWString((*iter)->getData().name)).c_str());
    }
    mapToArchive(*iter, path.substr().append(ToWString((*iter)->getData().name)), data);
  }
}


void InstallationManager::mapToArchive(const DirectoryTree::Node *baseNode)
{
  FileData* const *data;
  size_t size;
  m_CurrentArchive->getFileList(data, size);

  // first disable all files + folders, we will re-enable those present in baseNode
  for (size_t i = 0; i < size; ++i) {
    data[i]->setSkip(true);
  }

  std::wstring currentPath;

  mapToArchive(baseNode, currentPath, data);
}


bool InstallationManager::unpackSingleFile(const QString &fileName)
{
  FileData* const *data;
  size_t size;
  m_CurrentArchive->getFileList(data, size);

  QString baseName = QFileInfo(fileName).fileName();

  bool available = false;
  for (size_t i = 0; i < size; ++i) {
    if (_wcsicmp(data[i]->getFileName(), ToWString(fileName).c_str()) == 0) {
      available = true;
      data[i]->setSkip(false);
      data[i]->setOutputFileName(ToWString(baseName).c_str());
      m_TempFilesToDelete.insert(baseName);
    } else {
      data[i]->setSkip(true);
    }
  }

  if (available) {
    m_InstallationProgress.setWindowTitle(tr("Extracting files"));
    m_InstallationProgress.setLabelText(QString());
    m_InstallationProgress.setValue(0);
    m_InstallationProgress.setWindowModality(Qt::WindowModal);
    m_InstallationProgress.show();

    bool res = m_CurrentArchive->extract(ToWString(QDir::toNativeSeparators(QDir::tempPath())).c_str(),
                                  new MethodCallback<InstallationManager, void, float>(this, &InstallationManager::updateProgress),
                                  new MethodCallback<InstallationManager, void, LPCWSTR>(this, &InstallationManager::dummyProgressFile),
                                  new MethodCallback<InstallationManager, void, LPCWSTR>(this, &InstallationManager::report7ZipError));

    m_InstallationProgress.hide();

    return res;
  } else {
    return false;
  }
}


QString InstallationManager::extractFile(const QString &fileName)
{
  if (unpackSingleFile(fileName)) {
    QString tempFileName = QDir::tempPath().append("/").append(QFileInfo(fileName).fileName());

    m_FilesToDelete.insert(tempFileName);

    return tempFileName;
  } else {
    return QString();
  }
}


QString canonicalize(const QString &name)
{
  QString result(name);
  if ((result.startsWith('/')) ||
      (result.startsWith('\\'))) {
    result.remove(0, 1);
  }
  result.replace('/', '\\');

  return result;
}


QStringList InstallationManager::extractFiles(const QStringList &filesOrig)
{
  QStringList files;

  foreach (const QString &file, filesOrig) {
    files.append(canonicalize(file));
  }

  QStringList result;

  FileData* const *data;
  size_t size;
  m_CurrentArchive->getFileList(data, size);

  for (size_t i = 0; i < size; ++i) {
    if (files.contains(ToQString(data[i]->getFileName()), Qt::CaseInsensitive)) {
      const wchar_t *baseName = wcsrchr(data[i]->getFileName(), '\\');
      if (baseName == NULL) {
        baseName = wcsrchr(data[i]->getFileName(), '/');
      }
      if (baseName == NULL) {
        qCritical("failed to find backslash in %ls", data[i]->getFileName());
        continue;
      }
      data[i]->setOutputFileName(baseName);

      result.append(QDir::tempPath().append("/").append(ToQString(baseName)));

      data[i]->setSkip(false);
      m_TempFilesToDelete.insert(ToQString(baseName));
    } else {
      data[i]->setSkip(true);
    }
  }

  m_InstallationProgress.setWindowTitle(tr("Extracting files"));
  m_InstallationProgress.setLabelText(QString());
  m_InstallationProgress.setValue(0);
  m_InstallationProgress.setWindowModality(Qt::WindowModal);
  m_InstallationProgress.show();

  // unpack only the files we need for the installer
  if (!m_CurrentArchive->extract(ToWString(QDir::toNativeSeparators(QDir::tempPath())).c_str(),
         new MethodCallback<InstallationManager, void, float>(this, &InstallationManager::updateProgress),
         new MethodCallback<InstallationManager, void, LPCWSTR>(this, &InstallationManager::dummyProgressFile),
         new MethodCallback<InstallationManager, void, LPCWSTR>(this, &InstallationManager::report7ZipError))) {
    throw std::runtime_error("extracting failed");
  }

  m_InstallationProgress.hide();
  return result;
}

IPluginInstaller::EInstallResult InstallationManager::installArchive(GuessedValue<QString> &modName, const QString &archiveName)
{
  GuessedValue<QString> temp(modName);
  bool iniTweaks;
  if (install(archiveName, temp, iniTweaks)) {
    return IPluginInstaller::RESULT_SUCCESS;
  } else {
    return IPluginInstaller::RESULT_FAILED;
  }
}


DirectoryTree *InstallationManager::createFilesTree()
{
  FileData* const *data;
  size_t size;
  m_CurrentArchive->getFileList(data, size);

  QScopedPointer<DirectoryTree> result(new DirectoryTree);

  for (size_t i = 0; i < size; ++i) {
    // the files are in a flat list where each file has a a full path relative to the archive root
    // to create a tree, we have to iterate over each path component of each. This could be sped up by
    // grouping the filenames first, but so far there doesn't seem to be an actual performance problem
    DirectoryTree::Node *currentNode = result.data();

    QString fileName = ToQString(data[i]->getFileName());
    QStringList components = fileName.split("\\");

    // iterate over all path-components of this filename (including the filename itself)
    for (QStringList::iterator componentIter = components.begin(); componentIter != components.end(); ++componentIter) {
      if (componentIter->size() == 0) {
        // empty string indicates fileName is actually only a directory name and we have
        // completely processed it already.
        break;
      }

      bool exists = false;
      // test if this path is already in the tree
      for (DirectoryTree::node_iterator nodeIter = currentNode->nodesBegin(); nodeIter != currentNode->nodesEnd(); ++nodeIter) {
        if ((*nodeIter)->getData().name == *componentIter) {
          currentNode = *nodeIter;
          exists = true;
          break;
        }
      }

      if (!exists) {
        if (componentIter + 1 == components.end()) {
          // last path component. directory or file?
          if (data[i]->isDirectory()) {
            // this is a bit problematic. archives will often only list directories if they are empty,
            // otherwise the dir only appears in the path of a file. In the UI however we allow the user
            // to uncheck all files in a directory while keeping the dir checked. Those directories are
            // currently not installed.
            DirectoryTree::Node *newNode = new DirectoryTree::Node;
            newNode->setData(DirectoryTreeInformation(*componentIter, i));
            currentNode->addNode(newNode, false);
            currentNode = newNode;
          } else {
            currentNode->addLeaf(FileTreeInformation(*componentIter, i));
          }
        } else {
          DirectoryTree::Node *newNode = new DirectoryTree::Node;
          newNode->setData(DirectoryTreeInformation(*componentIter, -1));
          currentNode->addNode(newNode, false);
          currentNode = newNode;
        }
      }
    }
  }

  return result.take();
}


bool InstallationManager::isSimpleArchiveTopLayer(const DirectoryTree::Node *node, bool bainStyle)
{
  // see if there is at least one directory that makes sense on the top level
  for (DirectoryTree::const_node_iterator iter = node->nodesBegin(); iter != node->nodesEnd(); ++iter) {
    if ((bainStyle && InstallationTester::isTopLevelDirectoryBain((*iter)->getData().name)) ||
        (!bainStyle && InstallationTester::isTopLevelDirectory((*iter)->getData().name))) {
      qDebug("%s on the top level", (*iter)->getData().name.toUtf8().constData());
      return true;
    }
  }

  // see if there is a file that makes sense on the top level
  for (DirectoryTree::const_leaf_iterator iter = node->leafsBegin(); iter != node->leafsEnd(); ++iter) {
    if (InstallationTester::isTopLevelSuffix(iter->getName())) {
      return true;
    }
  }
  return false;
}


DirectoryTree::Node *InstallationManager::getSimpleArchiveBase(DirectoryTree *dataTree)
{
  DirectoryTree::Node *currentNode = dataTree;

  while (true) {
    if (isSimpleArchiveTopLayer(currentNode, false)) {
      return currentNode;
    } else if ((currentNode->numLeafs() == 0) &&
               (currentNode->numNodes() == 1)) {
      currentNode = *currentNode->nodesBegin();
    } else {
      qDebug("not a simple archive");
      return NULL;
    }
  }
}


void InstallationManager::updateProgress(float percentage)
{
  m_InstallationProgress.setValue(static_cast<int>(percentage * 100.0));
  if (m_InstallationProgress.wasCanceled()) {
    m_CurrentArchive->cancel();
    m_InstallationProgress.reset();
  }
}


void InstallationManager::updateProgressFile(LPCWSTR fileName)
{
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)
  m_InstallationProgress.setLabelText(QString::fromWCharArray(fileName));
#else
  m_InstallationProgress.setLabelText(QString::fromUtf16(fileName));
#endif
}


void InstallationManager::report7ZipError(LPCWSTR errorMessage)
{
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)
  m_InstallationProgress.setLabelText(QString::fromWCharArray(errorMessage));
#else
  reportError(QString::fromUtf16(errorMessage));
#endif
}


QString InstallationManager::generateBackupName(const QString &directoryName) const
{
  QString backupName = directoryName + "_backup";
  if (QDir(backupName).exists()) {
    int idx = 2;
    QString temp = backupName + QString::number(idx);
    while (QDir(temp).exists()) {
      ++idx;
      temp = backupName + QString::number(idx);
    }
    backupName = temp;
  }
  return backupName;
}


bool InstallationManager::testOverwrite(GuessedValue<QString> &modName) const
{
  QString targetDirectory = QDir::fromNativeSeparators(m_ModsDirectory.mid(0).append("\\").append(modName));

  while (QDir(targetDirectory).exists()) {
    QueryOverwriteDialog overwriteDialog(m_ParentWidget);
    if (overwriteDialog.exec()) {
      if (overwriteDialog.backup()) {
        QString backupDirectory = generateBackupName(targetDirectory);
        if (!copyDir(targetDirectory, backupDirectory, false)) {
          reportError(tr("failed to create backup"));
          return false;
        }
      }
      if (overwriteDialog.action() == QueryOverwriteDialog::ACT_RENAME) {
        bool ok = false;
        QString name = QInputDialog::getText(m_ParentWidget, tr("Mod Name"), tr("Name"),
                                             QLineEdit::Normal, modName, &ok);
        if (ok && !name.isEmpty()) {
          modName.update(name, GUESS_USER);
          if (!ensureValidModName(modName)) {
            return false;
          }
          targetDirectory = QDir::fromNativeSeparators(m_ModsDirectory.mid(0).append("\\").append(modName));
        }
      } else if (overwriteDialog.action() == QueryOverwriteDialog::ACT_REPLACE) {
        // save original settings like categories. Because it makes sense
        QString metaFilename = targetDirectory.mid(0).append("/meta.ini");
        QFile settingsFile(metaFilename);
        QByteArray originalSettings;
        if (settingsFile.open(QIODevice::ReadOnly)) {
          originalSettings = settingsFile.readAll();
          settingsFile.close();
        }

        // remove the directory with all content, then recreate it empty
        shellDelete(QStringList(targetDirectory));
        if (!QDir().mkdir(targetDirectory)) {
          // windows may keep the directory around for a moment, preventing its re-creation. Not sure
          // if this still happens with shellDelete
          Sleep(100);
          QDir().mkdir(targetDirectory);
        }
        // restore the saved settings
        if (settingsFile.open(QIODevice::WriteOnly)) {
          settingsFile.write(originalSettings);
          settingsFile.close();
        } else {
          qCritical("failed to restore original settings: %s", metaFilename.toUtf8().constData());
        }
        return true;
      } else if (overwriteDialog.action() == QueryOverwriteDialog::ACT_MERGE) {
        return true;
      }
    } else {
      return false;
    }
  }

  QDir().mkdir(targetDirectory);

  return true;
}


bool InstallationManager::ensureValidModName(GuessedValue<QString> &name) const
{
  while (name->isEmpty()) {
    bool ok;
    name.update(QInputDialog::getText(m_ParentWidget, tr("Invalid name"),
                                      tr("The name you entered is invalid, please enter a different one."),
                                      QLineEdit::Normal, "", &ok),
                GUESS_USER);
    if (!ok) {
      return false;
    }
  }
  return true;
}


bool InstallationManager::doInstall(GuessedValue<QString> &modName, int modID,
                                    const QString &version, const QString &newestVersion, int categoryID)
{
  if (!ensureValidModName(modName)) {
    return false;
  }

  // determine target directory
  if (!testOverwrite(modName)) {
    return false;
  }

  QString targetDirectory = QDir::fromNativeSeparators(m_ModsDirectory.mid(0).append("\\").append(modName));

  qDebug("installing to \"%s\"", targetDirectory.toUtf8().constData());

  m_InstallationProgress.setWindowTitle(tr("Extracting files"));
  m_InstallationProgress.setLabelText(QString());
  m_InstallationProgress.setValue(0);
  m_InstallationProgress.setWindowModality(Qt::WindowModal);
  m_InstallationProgress.show();

  if (!m_CurrentArchive->extract(ToWString(QDir::toNativeSeparators(targetDirectory)).c_str(),
         new MethodCallback<InstallationManager, void, float>(this, &InstallationManager::updateProgress),
         new MethodCallback<InstallationManager, void, LPCWSTR>(this, &InstallationManager::updateProgressFile),
         new MethodCallback<InstallationManager, void, LPCWSTR>(this, &InstallationManager::report7ZipError))) {
    if (m_CurrentArchive->getLastError() == Archive::ERROR_EXTRACT_CANCELLED) {
      return false;
    } else {
      throw std::runtime_error("extracting failed");
    }
  }

  m_InstallationProgress.hide();

  QSettings settingsFile(targetDirectory.mid(0).append("/meta.ini"), QSettings::IniFormat);

  // overwrite settings only if they are actually are available or haven't been set before
  if ((modID != 0) || !settingsFile.contains("modid")) {
    settingsFile.setValue("modid", modID);
  }
  if (!settingsFile.contains("version") ||
      (!version.isEmpty() &&
       (VersionInfo(version) >= VersionInfo(settingsFile.value("version").toString())))) {
    settingsFile.setValue("version", version);
  }
  if (!newestVersion.isEmpty() || !settingsFile.contains("newestVersion")) {
    settingsFile.setValue("newestVersion", newestVersion);
  }
  // issue #51 used to overwrite the manually set categories
  if (!settingsFile.contains("category")) {
    settingsFile.setValue("category", QString::number(categoryID));
  }
  settingsFile.setValue("installationFile", m_CurrentFile);

  return true;
}


void InstallationManager::openFile(const QString &fileName)
{
  unpackSingleFile(fileName);

  QString tempFileName = QDir::tempPath().append("/").append(QFileInfo(fileName).fileName());

  SHELLEXECUTEINFOW execInfo;
  memset(&execInfo, 0, sizeof(SHELLEXECUTEINFOW));
  execInfo.cbSize = sizeof(SHELLEXECUTEINFOW);
  execInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
  execInfo.lpVerb = L"open";
  std::wstring fileNameW = ToWString(tempFileName);
  execInfo.lpFile = fileNameW.c_str();
  execInfo.nShow = SW_SHOWNORMAL;
  if (!::ShellExecuteExW(&execInfo)) {
    qCritical("failed to spawn %s: %d", tempFileName.toUtf8().constData(), ::GetLastError());
  }

  m_FilesToDelete.insert(tempFileName);
}


// copy and pasted from mo_dll
bool EndsWith(LPCWSTR string, LPCWSTR subString)
{
  size_t slen = wcslen(string);
  size_t len = wcslen(subString);
  if (slen < len) {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    if (towlower(string[slen - len + i]) != towlower(subString[i])) {
      return false;
    }
  }
  return true;
}


bool InstallationManager::wasCancelled()
{
  return m_CurrentArchive->getLastError() == Archive::ERROR_EXTRACT_CANCELLED;
}


bool InstallationManager::install(const QString &fileName, GuessedValue<QString> &modName, bool &hasIniTweaks)
{
  QFileInfo fileInfo(fileName);
  if (m_SupportedExtensions.find(fileInfo.suffix()) == m_SupportedExtensions.end()) {
    reportError(tr("File format \"%1\" not supported").arg(fileInfo.completeSuffix()));
    return false;
  }

  modName.setFilter(&fixDirectoryName);

  modName.update(QFileInfo(fileName).completeBaseName(), GUESS_FALLBACK);

  // read out meta information from the download if available
  int modID = 0;
  QString version = "";
  QString newestVersion = "";
  int categoryID = 0;

  QString metaName = fileName.mid(0).append(".meta");
  if (QFile(metaName).exists()) {
    QSettings metaFile(metaName, QSettings::IniFormat);
    modID = metaFile.value("modID", 0).toInt();
    modName.update(metaFile.value("name", "").toString(), GUESS_FALLBACK);
    modName.update(metaFile.value("modName", "").toString(), GUESS_META);

    version = metaFile.value("version", "").toString();
    newestVersion = metaFile.value("newestVersion", "").toString();
    unsigned int categoryIndex = CategoryFactory::instance().resolveNexusID(metaFile.value("category", 0).toInt());
    categoryID = CategoryFactory::instance().getCategoryID(categoryIndex);
  }

  if (version.isEmpty()) {
    QDateTime lastMod = fileInfo.lastModified();
    version = "d" + lastMod.toString("yyyy.M.d");
  }

  { // guess the mod name and mod if from the file name if there was no meta information
    QString guessedModName;
    int guessedModID = modID;
    NexusInterface::interpretNexusFileName(QFileInfo(fileName).baseName(), guessedModName, guessedModID, false);
    if ((modID == 0) && (guessedModID != -1)) {
      modID = guessedModID;
    } else if (modID != guessedModID) {
      qDebug("passed mod id: %d, guessed id: %d", modID, guessedModID);
    }
    modName.update(guessedModName, GUESS_GOOD);
  }

  qDebug("using mod name \"%s\" (id %d)", modName->toUtf8().constData(), modID);
  m_CurrentFile = fileInfo.fileName();

  // open the archive and construct the directory tree the installers work on
  bool archiveOpen = m_CurrentArchive->open(ToWString(QDir::toNativeSeparators(fileName)).c_str(),
                                            new MethodCallback<InstallationManager, void, LPSTR>(this, &InstallationManager::queryPassword));

  ON_BLOCK_EXIT([this] {
    this->m_CurrentArchive->close();
  });

  DirectoryTree *filesTree = archiveOpen ? createFilesTree() : NULL;

  IPluginInstaller::EInstallResult installResult = IPluginInstaller::RESULT_NOTATTEMPTED;

  std::sort(m_Installers.begin(), m_Installers.end(), [] (IPluginInstaller *LHS, IPluginInstaller *RHS) {
            return LHS->priority() > RHS->priority();
      });

  foreach (IPluginInstaller *installer, m_Installers) {
    // don't use inactive installers
    if (!installer->isActive()) {
      continue;
    }

    // try only manual installers if that was requested
    if ((installResult == IPluginInstaller::RESULT_MANUALREQUESTED) && !installer->isManualInstaller()) {
      continue;
    }

    try {
      { // simple case
        IPluginInstallerSimple *installerSimple = dynamic_cast<IPluginInstallerSimple*>(installer);
        if ((installerSimple != NULL) &&
            (filesTree != NULL) && (installer->isArchiveSupported(*filesTree))) {
          installResult = installerSimple->install(modName, *filesTree, version, modID);
          if (installResult == IPluginInstaller::RESULT_SUCCESS) {
            mapToArchive(filesTree);
            // the simple installer only prepares the installation, the rest works the same for all installers
            if (!doInstall(modName, modID, version, newestVersion, categoryID)) {
              installResult = IPluginInstaller::RESULT_FAILED;
            }
          }
        }
      }

      { // custom case
        IPluginInstallerCustom *installerCustom = dynamic_cast<IPluginInstallerCustom*>(installer);
        if ((installerCustom != NULL) &&
            (((filesTree != NULL) && installer->isArchiveSupported(*filesTree)) ||
             ((filesTree == NULL) && installerCustom->isArchiveSupported(fileName)))) {
          std::set<QString> installerExtensions = installerCustom->supportedExtensions();
          if (installerExtensions.find(fileInfo.suffix()) != installerExtensions.end()) {
            installResult = installerCustom->install(modName, fileName, version, modID);
          }
        }
      }
    } catch (const IncompatibilityException &e) {
      qCritical("plugin \"%s\" incompatible: %s",
                qPrintable(installer->name()), e.what());
    }

    // act upon the installation result. at this point the files have already been
    // extracted to the correct location
    switch (installResult) {
      case IPluginInstaller::RESULT_CANCELED:
      case IPluginInstaller::RESULT_FAILED: {
        return false;
      } break;
      case IPluginInstaller::RESULT_SUCCESS: {
        if (filesTree != NULL) {
          DirectoryTree::node_iterator iniTweakNode = filesTree->nodeFind(DirectoryTreeInformation("INI Tweaks"));
          hasIniTweaks = (iniTweakNode != filesTree->nodesEnd()) &&
                         ((*iniTweakNode)->numLeafs() != 0);
          return true;
        } else {
          return false;
        }
      } break;
    }
  }

  reportError(tr("None of the available installer plugins were able to handle that archive"));
  return false;
}



QString InstallationManager::getErrorString(Archive::Error errorCode)
{
  switch (errorCode) {
    case Archive::ERROR_NONE: {
      return tr("no error");
    } break;
    case Archive::ERROR_LIBRARY_NOT_FOUND: {
      return tr("7z.dll not found");
    } break;
    case Archive::ERROR_LIBRARY_INVALID: {
      return tr("7z.dll isn't valid");
    } break;
    case Archive::ERROR_ARCHIVE_NOT_FOUND: {
      return tr("archive not found");
    } break;
    case Archive::ERROR_FAILED_TO_OPEN_ARCHIVE: {
      return tr("failed to open archive");
    } break;
    case Archive::ERROR_INVALID_ARCHIVE_FORMAT: {
      return tr("unsupported archive type");
    } break;
    case Archive::ERROR_LIBRARY_ERROR: {
      return tr("internal library error");
    } break;
    case Archive::ERROR_ARCHIVE_INVALID: {
      return tr("archive invalid");
    } break;
    default: {
      // this probably means the archiver.dll is newer than this
      return tr("unknown archive error");
    } break;
  }
}


void InstallationManager::registerInstaller(IPluginInstaller *installer)
{
  m_Installers.push_back(installer);
  installer->setInstallationManager(this);
  IPluginInstallerCustom *installerCustom = dynamic_cast<IPluginInstallerCustom*>(installer);
  if (installerCustom != NULL) {
    std::set<QString> extensions = installerCustom->supportedExtensions();
    m_SupportedExtensions.insert(extensions.begin(), extensions.end());
  }
}

QStringList InstallationManager::getSupportedExtensions() const
{
  QStringList result;
  foreach (const QString &extension, m_SupportedExtensions) {
    result.append(extension);
  }
  return result;
}