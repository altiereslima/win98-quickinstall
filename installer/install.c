/*
 * LUNMERCY - Installer component
 * (C) 2023 Eric Voirin (oerg866@googlemail.com)
 */

#include "install.h"

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <utime.h>
#include <unistd.h>
#include <dirent.h>
#include <locale.h>

#include "qi_assert.h"
#include "mappedfile.h"
#include "util.h"
#include "version.h"

#include "anbui/anbui.h"


#define CD_FILE_PATH_ROOT   (0)

typedef enum {
    INSTALL_WELCOME = 0,
    INSTALL_OSROOT_VARIANT_SELECT,
    INSTALL_MAIN_MENU,
    INSTALL_PARTITION_WIZARD,
    INSTALL_SELECT_DESTINATION_PARTITION,
    INSTALL_FORMAT_PARTITION_PROMPT,
    INSTALL_MBR_ACTIVE_BOOT_PROMPT,
    INSTALL_REGISTRY_VARIANT_PROMPT,
    INSTALL_INTEGRATED_DRIVERS_PROMPT,
    INSTALL_DO_INSTALL,
} inst_InstallStep;


#pragma pack(1)

typedef struct {
    uint8_t fileFlags;
    uint16_t fileDate;
    uint16_t fileTime;
    uint32_t fileSize;
    int32_t fileno;
} inst_MercyPakFileDescriptor;

#pragma pack()

// -sizeof(int) because the fileno is not part of the descriptor read from the mercypak file
#define MERCYPAK_FILE_DESCRIPTOR_SIZE (sizeof(inst_MercyPakFileDescriptor) - sizeof(int))
// -sizeof(uint32_t) because the filesize is not part of the descriptor read from the mercypak v2 file
#define MERCYPAK_V2_FILE_DESCRIPTOR_SIZE ((MERCYPAK_FILE_DESCRIPTOR_SIZE) - sizeof(uint32_t))

#define MERCYPAK_V2_MAX_IDENTICAL_FILES (16)

#define MERCYPAK_V1_MAGIC "ZIEG"
#define MERCYPAK_V2_MAGIC "MRCY"

#define INST_CFDISK_CMD "cfdisk "
#define INST_COLS (74)

#define INST_SYSROOT_FILE "FULL.866"
#define INST_DRIVER_FILE  "DRIVER.866"
#define INST_SLOWPNP_FILE "SLOWPNP.866"
#define INST_FASTPNP_FILE "FASTPNP.866"

#define INST_CDROM_IO_SIZE (512*1024)
#define INST_DISK_IO_SIZE (512*1024)

static const char *cdrompath = NULL;    // Path to install source media
static const char *cdromdev = NULL;     // Block device for install source media
                                        // ^ initialized in install_main

/* Gets the absolute CDROM path of a file. 
   osVariantIndex is the index for the source variant, 0 means from the root. */
static const char *inst_getCDFilePath(size_t osVariantIndex, const char *filepath) {
    static char staticPathBuf[1024];
    if (osVariantIndex > 0) {
        snprintf(staticPathBuf, sizeof(staticPathBuf), "%s/osroots/%zu/%s", cdrompath, osVariantIndex, filepath);
    } else {
        snprintf(staticPathBuf, sizeof(staticPathBuf), "%s/%s", cdrompath, filepath);
    }
    return staticPathBuf;
}

/* Opens a file from the source media. osVariantIndex is the index for the source variant, 0 means from the root. */
static inline MappedFile *inst_openSourceFile(size_t osVariantIndex, const char *filename, size_t readahead) {
    return mappedFile_open(inst_getCDFilePath(osVariantIndex, filename), readahead);
}

/* Shows disclaimer text */
static inline void inst_showDisclaimer() {
    ad_textFileBox("INSTALAÇÃO", inst_getCDFilePath(0, "install.txt"));
}

/* Shows welcome screen, returns false if user wants to exit to shell */
static inline void inst_showWelcomeScreen() {
    ad_okBox("Bem-vindo", false, "Bem-vindo ao Instalador do Windows!");
}

/* Checks if given hard disk contains the installation source */
static inline bool inst_isInstallationSourceDisk(util_HardDisk *disk) {
    return (util_stringStartsWith(cdromdev, disk->device));
}

/* Checks if given partition is the installation source */
static inline bool inst_isInstallationSourcePartition(util_Partition *part) {
    return (util_stringEquals(cdromdev, part->device));
}

/* Tells the user he is trying to partition the install source disk */
static inline void inst_showInstallationSourceDiskError() {
    ad_okBox("Atenção", false, "O disco selecionado contém a fonte de instalação.\nEle não pode ser particionado.");
}

/* Tells the user he is trying to install to the install source partition */
static inline void inst_showInstallationSourcePartitionError() {
    ad_okBox("Atenção", false, "A partição selecionada contém a fonte de instalação.\nEla não pode ser o destino da instalação.");
}

/* Tells the user he is trying to install to a non-FAT partition */
static inline void inst_showUnsupportedFileSystemError() {
    ad_okBox("Atenção", false, "A partição selecionada tem um sistema de arquivos não suportado.\nEla não pode ser o destino da instalação.");
}

/* Tells the user he is trying to install to a computer without hard disks. */
static inline void inst_noHardDisksFoundError() {
    ad_okBox("Atenção", false, "Nenhum disco rígido encontrado!\nPor favor, instale um disco rígido e tente novamente!");
}

/* Tells the user about an oopsie trying to open a file for reading. */
static inline void inst_showFileError() {
    ad_okBox("Atenção", false, "ERRO: Ocorreu um problema ao lidar com um arquivo para esta variante do SO.\n(%d: %s)", errno, strerror(errno));
}

static inline util_HardDiskArray *inst_getSystemHardDisks() {
    ad_setFooterText("Obtendo informações sobre os discos rígidos do sistema...");
    util_HardDiskArray *ret = util_getSystemHardDisks();
    ad_clearFooter();
    return ret;
}

typedef enum {
    SETUP_ACTION_INSTALL = 0,
    SETUP_ACTION_PARTITION_WIZARD,
    SETUP_ACTION_EXIT_TO_SHELL,
} inst_SetupAction;

static inst_SetupAction inst_showMainMenu() {
    ad_Menu *menu = ad_menuCreate("Instalador do Windows 9x: Menu Principal", "Para onde você quer ir hoje?", true);
    QI_ASSERT(menu);

ad_menuAddItemFormatted(menu, "[INSTALAR] Instalar a variante do Sistema Operacional selecionada");
ad_menuAddItemFormatted(menu, " [CFDISK] Particionar os discos rígidos");
ad_menuAddItemFormatted(menu, "  [SHELL] Sair para o shell de diagnóstico mínimo do Linux");

    int menuResult = ad_menuExecute(menu);

    QI_ASSERT(menuResult != AD_ERROR);

    if (menuResult == AD_CANCELED) /* Canceled = exit to shell for us */
        menuResult = (int) SETUP_ACTION_EXIT_TO_SHELL;

    ad_menuDestroy(menu);

    return (inst_SetupAction) menuResult;
}

/* 
    Shows OS Variant select. This is a bit peculiar because the dialog is not shown if
    there is only one OS variant choice. In that case it always returns "true".
    Otherwise it can return "false" if the user selected BACK.
*/
static bool inst_showOSVariantSelect(size_t *variantIndex, size_t *variantCount) {
    ad_Menu *menu = ad_menuCreate("Variante de Instalação", "Selecione a variante do sistema operacional que deseja instalar.", true);
    int menuResult;

    QI_ASSERT(menu);

    *variantCount = 0;

    // Find and get available OS variants.

    while (true) {
        char tmpInfPath[256];
        char tmpVariantLabel[128];

        snprintf(tmpInfPath, sizeof(tmpInfPath), "%s/osroots/%zu/win98qi.inf", cdrompath, (*variantCount) + 1); // Variant index starts at 1

        if (!util_fileExists(tmpInfPath)) {
            break;
        }

        if (!util_readFirstLineFromFileIntoBuffer(tmpInfPath, tmpVariantLabel, sizeof(tmpVariantLabel))) {
            ad_okBox("Erro", false, "Erro ao ler o arquivo\n'%s'", tmpInfPath);
            break;
        }

        ad_menuAddItemFormatted(menu, "%zu: %s", (*variantCount) + 1, tmpVariantLabel);

        *variantCount += 1;

    }

    if (*variantCount > 1) {
        menuResult = ad_menuExecute(menu);
    } else {
        // Don't have to show a menu if we have no choice to do innit.
        menuResult = 0;
    }

    ad_menuDestroy(menu);

    QI_ASSERT(menuResult != AD_ERROR);

    if (menuResult == AD_CANCELED) {
        // BACK was pressed.
        return false;
    } else {
        *variantIndex = (size_t) menuResult + 1; // Just in case the directory listing is out of order for some reason...
        return true;   
    }    
}

/* Show partition wizard, returns true if user finished or false if he selected BACK. */
static void inst_showPartitionWizard(util_HardDiskArray *hdds) {
    char cfdiskCmd[UTIL_MAX_CMD_LENGTH];
    int menuResult;

    QI_ASSERT(hdds);

    if (hdds->count == 0) {
        inst_noHardDisksFoundError();
        return;
    }

    while (1) {
        ad_Menu *menu = ad_menuCreate("Assistente de Partição", "Selecione o Disco Rígido que você deseja particionar.", true);

        QI_ASSERT(menu);

        for (size_t i = 0; i < hdds->count; i++) {
            ad_menuAddItemFormatted(menu, "%s [%s] - Tamanho: %llu MB %s",
                hdds->disks[i].device,
                hdds->disks[i].model,
                hdds->disks[i].size / 1024ULL / 1024ULL,
                inst_isInstallationSourceDisk(&hdds->disks[i]) ? "(*)" : ""
                );
        }
        
        ad_menuAddItemFormatted(menu, "%s", "[FINALIZADO]");

        menuResult = ad_menuExecute(menu);

        ad_menuDestroy(menu);

        QI_ASSERT(menuResult != AD_ERROR);

        if (menuResult == AD_CANCELED) // BACK was pressed.
            return;
        
        if (menuResult == (int) hdds->count)
            return;
        
        // Check if we're trying to partition the install source disk. If so, warn user and continue looping.
        if (inst_isInstallationSourceDisk(&hdds->disks[menuResult])) {
            inst_showInstallationSourceDiskError();
            continue;
        }

        // Invoke cfdisk command for chosen drive.
        snprintf(cfdiskCmd, UTIL_MAX_CMD_LENGTH, "%s%s", INST_CFDISK_CMD, hdds->disks[menuResult].device);      
        system(cfdiskCmd);

        ad_restore();
        ad_okBox("Atenção", false,
            "Lembre-se de responder 'sim' ao prompt de formatação\n"
            "se estiver instalando em uma partição que acabou de criar!");
    }
}

/* Shows partition selector. Returns pointer to the selected partition. A return value of NULL means the user wants to go back. */
static util_Partition *inst_showPartitionSelector(util_HardDiskArray *hdds) {
    int menuResult;
    util_Partition *result = NULL;

    QI_ASSERT(hdds);

    if (hdds->count == 0) {
        inst_noHardDisksFoundError();
        return NULL;
    }

    while (1) {
        ad_Menu *menu = ad_menuCreate("Destino da Instalação", "Selecione a partição para a qual você deseja instalar.", true);

        QI_ASSERT(menu);

        for (size_t disk = 0; disk < hdds->count; disk++) {
            util_HardDisk *harddisk = &hdds->disks[disk];

            for (size_t part = 0; part < harddisk->partitionCount; part++) {
                util_Partition *partition = &harddisk->partitions[part];

                ad_menuAddItemFormatted(menu, "%8s: (%s, %llu MB) no disco %s [%s] %s",
                    util_shortDeviceString(partition->device),
                    util_utilFilesystemToString(partition->fileSystem),
                    partition->size / 1024ULL / 1024ULL,
                    util_shortDeviceString(harddisk->device),
                    harddisk->model,
                    inst_isInstallationSourcePartition(partition) ? "(*)" : ""
                );
            }
        }

        if (ad_menuGetItemCount(menu) == 0) {
            ad_menuDestroy(menu);
            ad_okBox("Erro", false, "Nenhuma partição foi encontrada! Particione o disco e tente novamente!");
            return NULL;
        }

        menuResult = ad_menuExecute(menu);
        ad_menuDestroy(menu);
        
        if (menuResult < 0) { // User wants to go back
            return NULL;
        }

        result = util_getPartitionFromIndex(hdds, menuResult);
        QI_ASSERT(result);

        // Is this the installation source? If yes, show menu again.
        if (inst_isInstallationSourcePartition(result)) {
            inst_showInstallationSourcePartitionError();
            continue;
        }
        
        if (result->fileSystem == fs_unsupported || result->fileSystem == fs_none) {
            inst_showUnsupportedFileSystemError();
            continue;
        }

        return result;
    }
}

/* Asks user if he wants to format selected partition. Returns true if so. */
static inline int inst_formatPartitionDialog(util_Partition *part) {
    return ad_yesNoBox("Confirmar", true,
        "Você escolheu a partição '%s'.\n"
        "Gostaria de formatá-la antes da instalação (recomendado)?\n",
        part->device);
}

static bool inst_formatPartition(util_Partition *part) {
    char formatCmd[UTIL_MAX_CMD_LENGTH];
    bool ret = util_getFormatCommand(part, part->fileSystem, formatCmd, UTIL_MAX_CMD_LENGTH);
    QI_ASSERT(ret && "GetFormatCommand");
    return (0 == ad_runCommandBox("Formatando partição...", formatCmd));
}

/* Asks user if he wants to overwrite the MBR and set the partition active. Returns true if so. */
static inline int inst_askUserToOverwriteMBRAndSetActive(util_Partition *part) {
    return ad_yesNoBox("Confirmar", true,
        "Você escolheu a partição '%s'.\n"
        "Gostaria de sobrescrever o Master Boot Record (MBR)\n"
        "e tornar a partição ativa (recomendado)?", 
        part->device);
}

/* Show message box informing user that formatting failed. */
static inline void inst_showFailedFormat(util_Partition *part) {
    ad_okBox("Erro", false,
        "A partição %s não pôde ser formatada.\n"
        "O último erro registrado foi: '%s'.\n"
        "Pode haver um problema com o disco.\n"
        "Tente particionar o disco novamente ou usar outra partição.\n"
        "Voltando para o seletor de partições.",
        part->device, strerror(errno));
}

/* Show message box informing user that mount failed*/
static inline void inst_showFailedMount(util_Partition *part) {
    ad_okBox("Erro", false,
        "A partição %s não pôde ser acessada.\n"
        "O último erro registrado foi: '%s'.\n"
        "Pode haver um problema com o disco.\n"
        "Você pode tentar formatar a partição.\n"
        "Voltando para o seletor de partições.",
        part->device, strerror(errno));
}

/* Show message box informing user that copying failed*/
static inline void inst_showFailedCopy(const char *sourceFile) {
    ad_okBox("Erro", false,
        "Ocorreu um erro ao descompactar '%s'\n"
        "para esta variante do sistema operacional.\n"
        "O último erro registrado foi: '%s'.\n"
        "Pode haver um problema com o disco.\n"
        "Você pode tentar usar outro disco de origem / destino.\n"
        "Voltando para o seletor de partições.", 
        sourceFile,
        strerror(errno));
}


/* Ask user if he wants to install driver package */
static inline int inst_showDriverPrompt() {
    return ad_yesNoBox("Seleção", true, "Você gostaria de instalar os drivers integrados?");
}

/* Gets a MercyPak string (8 bit length + n chars) into dst. Must be a buffer of >= 256 bytes size. */
static inline bool inst_getMercyPakString(MappedFile *file, char *dst) {
    bool success;
    uint8_t count;
    success = mappedFile_getUInt8(file, &count);
    success &= mappedFile_read(file, (uint8_t*) dst, (size_t) (count));
    dst[(size_t) count] = 0x00;
    return success;
}

static bool inst_copyFiles(MappedFile *file, const char *installPath, const char *filePromptString) {
    char fileHeader[5] = {0};
    char *destPath = malloc(strlen(installPath) + 256 + 1);   // Full path of destination dir/file, the +256 is because mercypak strings can only be 255 chars max
    char *destPathAppend = destPath + strlen(installPath) + 1;  // Pointer to first char after the base install path in the destination path + 1 for the extra "/" we're gonna append
    bool mercypakV2 = false;

    sprintf(destPath, "%s/", installPath);

    uint32_t dirCount;
    uint32_t fileCount;
    bool success = true;

    success &= mappedFile_read(file, (uint8_t*) fileHeader, 4);
    QI_ASSERT(success && "fileHeader");
    success &= mappedFile_getUInt32(file, &dirCount);
    QI_ASSERT(success && "dirCount");
    success &= mappedFile_getUInt32(file, &fileCount);
    QI_ASSERT(success && "fileCount");

    /* printf("File header: %s, dirs %d files: %d\n", fileHeader, (int) dirCount, (int) fileCount); */

    /* Check if we're unpacking a V2 file, which does redundancy stuff. */

    if (util_stringEquals(fileHeader, MERCYPAK_V1_MAGIC)) {
        mercypakV2 = false;
    } else if (util_stringEquals(fileHeader, MERCYPAK_V2_MAGIC)) {
        mercypakV2 = true;
    } else {
        QI_ASSERT(false && "Cabeçalho do arquivo incorreto");
        free(destPath);
        return false;
    }

    ad_ProgressBox *pbox = ad_progressBoxCreate("Instalador do Windows 9x", dirCount, "Criando Diretórios (%s)...", filePromptString);

    QI_ASSERT(pbox);

    for (uint32_t d = 0; d < dirCount; d++) {
        uint8_t dirFlags;

        ad_progressBoxUpdate(pbox, d);

        success &= mappedFile_getUInt8(file, &dirFlags);
        success &= inst_getMercyPakString(file, destPathAppend);
        util_stringReplaceChar(destPathAppend, '\\', '/'); // DOS paths innit
        success &= (mkdir(destPath, dirFlags) == 0 || (errno == EEXIST));    // An error value is ok if the directory already exists. It means we can write to it. IT'S FINE.
    }

    ad_progressBoxDestroy(pbox);

    /*
     *  Extract and copy files from mercypak files
     */

    success = true;

    pbox = ad_progressBoxCreate("Instalador do Windows 9x", mappedFile_getFileSize(file), "Copiando Arquivos (%s)...", filePromptString);

    QI_ASSERT(pbox);

    if (mercypakV2) {
        /* Handle mercypak v2 pack file with redundant files optimized out */

        inst_MercyPakFileDescriptor *filesToWrite = calloc(MERCYPAK_V2_MAX_IDENTICAL_FILES, sizeof(inst_MercyPakFileDescriptor));
        int *fileDescriptorsToWrite = calloc(MERCYPAK_V2_MAX_IDENTICAL_FILES, sizeof(int));
        uint8_t identicalFileCount = 0;

        QI_ASSERT(filesToWrite != NULL);

        for (uint32_t f = 0; f < fileCount;) {

            uint32_t fileSize;

            ad_progressBoxUpdate(pbox, mappedFile_getPosition(file));

            success &= mappedFile_getUInt8(file, &identicalFileCount);

            QI_ASSERT(identicalFileCount <= MERCYPAK_V2_MAX_IDENTICAL_FILES);

            for (uint32_t subFile = 0; subFile < identicalFileCount; subFile++) {
                success &= inst_getMercyPakString(file, destPathAppend);
                util_stringReplaceChar(destPathAppend, '\\', '/');

                fileDescriptorsToWrite[subFile] = open(destPath,  O_WRONLY | O_CREAT | O_TRUNC);
                QI_ASSERT(fileDescriptorsToWrite[subFile] >= 0);

                success &= mappedFile_read(file, &filesToWrite[subFile], MERCYPAK_V2_FILE_DESCRIPTOR_SIZE);
            }

            success &= mappedFile_getUInt32(file, &fileSize);

            success &= mappedFile_copyToFiles(file, identicalFileCount, fileDescriptorsToWrite, fileSize);

            for (uint32_t subFile = 0; subFile < identicalFileCount; subFile++) {
                success &= util_setDosFileTime(fileDescriptorsToWrite[subFile], filesToWrite[subFile].fileDate, filesToWrite[subFile].fileTime);
                success &= util_setDosFileAttributes(fileDescriptorsToWrite[subFile], filesToWrite[subFile].fileFlags);
                close(fileDescriptorsToWrite[subFile]);
            }

            f += identicalFileCount;
        }

        free(filesToWrite);
        free(fileDescriptorsToWrite);
    } else {

        inst_MercyPakFileDescriptor fileToWrite;

        for (uint32_t f = 0; f < fileCount; f++) {

            ad_progressBoxUpdate(pbox, mappedFile_getPosition(file));

            /* Mercypak file metadata (see mercypak.txt) */

            success &= inst_getMercyPakString(file, destPathAppend);    // First, filename string
            util_stringReplaceChar(destPathAppend, '\\', '/');          // DOS paths innit

            success &= mappedFile_read(file, &fileToWrite, MERCYPAK_FILE_DESCRIPTOR_SIZE);

            int outfd = open(destPath,  O_WRONLY | O_CREAT | O_TRUNC);
            QI_ASSERT(outfd >= 0);

            success &= mappedFile_copyToFiles(file, 1, &outfd, fileToWrite.fileSize);

            success &= util_setDosFileTime(outfd, fileToWrite.fileDate, fileToWrite.fileTime);
            success &= util_setDosFileAttributes(outfd, fileToWrite.fileFlags);

            close(outfd);

        }

    }

    /*
        TODO: ERROR HANDLING
     */

    ad_progressBoxDestroy(pbox);

    free(destPath);
    return success;
}

/* Inform user and setup boot sector and MBR. */
static bool inst_setupBootSectorAndMBR(util_Partition *part, bool setActiveAndDoMBR) {
    bool success = true;
    // TODO: ui_showInfoBox("Setting up Master Boot Record and Boot sector...");
    success &= util_writeWin98BootSectorToPartition(part);
    if (setActiveAndDoMBR) {
        success &= util_writeWin98MBRToDrive(part->parent);
        char activateCmd[UTIL_MAX_CMD_LENGTH];
        snprintf(activateCmd, UTIL_MAX_CMD_LENGTH, "sfdisk --activate %s %zu", part->parent->device, part->indexOnParent);
        success &= (0 == ad_runCommandBox("Ativando partição...", activateCmd));
    }
    return success;
}

/* Show success screen. Ask user if he wants to reboot */
static inline bool inst_showSuccessAndAskForReboot() {
    // Returns TRUE (meaning reboot = true) if YES (0) happens. sorry for the confusion.
    ad_Menu *menu = ad_menuCreate("Instalador do Windows 9x: Sucesso", 
        "A instalação foi bem-sucedida.\n"
        "Você gostaria de reiniciar ou sair para o shell?", 
        false);

    QI_ASSERT(menu);

    ad_menuAddItemFormatted(menu, "Reiniciar");
    ad_menuAddItemFormatted(menu, "Sair para o shell");

    int menuResult = ad_menuExecute(menu);

    ad_menuDestroy(menu);

    return menuResult == 0;
}

/* Show failure screen :( */
static inline void inst_showFailMessage() {
    ad_okBox("Erro!", false,
        "Houve um problema durante a instalação! :(\n"
        "Você pode pressionar ENTER para acessar o shell e inspecionar o problema.");
}

/* Asks user which version of the hardware detection scheme he wants */
static const char *inst_askUserForRegistryVariant(void) {
    const char *optionFiles[] = { 
        "FASTPNP.866", 
        "SLOWPNP.866"
    };
    const char *optionLabels[] = { 
        "Detecção rápida de hardware, pulando a maioria dos dispositivos não PNP.",
        "Detecção completa de hardware, incluindo TODOS os dispositivos não PNP."
    };

    int menuResult = ad_menuExecuteDirectly("Selecionar método de detecção de hardware", true, 
        util_arraySize(optionLabels), optionLabels, 
        "Por favor, selecione o método de detecção de hardware a ser usado.");

    if (menuResult == AD_CANCELED) {
        return NULL;
    }

    QI_ASSERT(menuResult < (int) util_arraySize(optionLabels));
    return optionFiles[menuResult];
}

/* Main installer process. Assumes the CDROM environment variable is set to a path with valid install.txt, FULL.866 and DRIVER.866 files. */
bool inst_main() {
    MappedFile *sourceFile = NULL;
    size_t readahead = util_getProcSafeFreeMemory() * 6 / 10;
    util_HardDiskArray *hda = NULL;
    const char *registryUnpackFile = NULL;
    util_Partition *destinationPartition = NULL;
    inst_InstallStep currentStep = INSTALL_WELCOME;
    size_t osVariantIndex = 0;

    bool installDrivers = false;
    bool formatPartition = false;
    bool setActiveAndDoMBR = false;
    bool quit = false;
    bool doReboot = false;
    bool installSuccess = true;
    bool goToNext = false;

    ad_init(LUNMERCY_BACKTITLE);

    setlocale(LC_ALL, "C.UTF-8");

    cdrompath = getenv("CDROM");
    cdromdev = getenv("CDDEV");

    QI_ASSERT(cdrompath);
    QI_ASSERT(cdromdev);

 
    inst_showDisclaimer();

    while (!quit) {

        /* Basic concept:
         *  If goToNext is true, we advance one step, if it is false, we go back one step.
            (Can be circumvented by using "continue")
         *  */

        switch (currentStep) {
            case INSTALL_WELCOME:
                inst_showWelcomeScreen();
                goToNext = true;
                break;

            case INSTALL_OSROOT_VARIANT_SELECT: {
                bool previousGoToNext = goToNext;
                size_t osVariantCount = 0;

                goToNext = inst_showOSVariantSelect(&osVariantIndex, &osVariantCount);

                if (osVariantCount == 1 && previousGoToNext == false) {
                    // If there's only one OS variant the variant select will just return "true"
                    // since the user doesn't get to press back we need to handle this here.
                    // so if the previous go to next was false we will go back outright.
                    goToNext = false;
                } else if (goToNext) {
                    if (sourceFile) {
                        mappedFile_close(sourceFile);
                        sourceFile = NULL;
                    }

                    sourceFile = inst_openSourceFile(osVariantIndex, INST_SYSROOT_FILE, readahead);

                    if (sourceFile == NULL) {
                        inst_showFileError();
                        continue;
                    }
                }

                break;
            }

            /* Main Menu:
             * Select Setup Action to execute */
            case INSTALL_MAIN_MENU: {
                switch (inst_showMainMenu()) {
                    case SETUP_ACTION_INSTALL:
                        currentStep = INSTALL_SELECT_DESTINATION_PARTITION;
                        continue;

                    case SETUP_ACTION_PARTITION_WIZARD:
                        currentStep = INSTALL_PARTITION_WIZARD;
                        continue;

                    case SETUP_ACTION_EXIT_TO_SHELL:
                        doReboot = false;
                        quit = true;
                        break;

                    default:
                        break;
                }

                continue;
            }

            /* Setup Action:
             * Partition wizard.
             * Select hard disk to partition. */
            case INSTALL_PARTITION_WIZARD: {
                // Go back to selector after this regardless what happens
                currentStep = INSTALL_MAIN_MENU;
                goToNext = false;

                util_hardDiskArrayDestroy(hda);
                hda = inst_getSystemHardDisks();

                QI_ASSERT(hda != NULL);

                inst_showPartitionWizard(hda);
                continue;
            }

            /* Setup Action:
             * Start installation.
             * Select the destination partition */
            case INSTALL_SELECT_DESTINATION_PARTITION: {
                util_hardDiskArrayDestroy(hda);
                hda = inst_getSystemHardDisks();

                QI_ASSERT(hda != NULL);

                destinationPartition = inst_showPartitionSelector(hda);

                if (destinationPartition == NULL) {
                    // There was an error, the user canceled or we have no hard disks.
                    // Either way, we have to go back.
                    currentStep = INSTALL_MAIN_MENU;
                    continue;
                }

                goToNext = true;
                break;
            }

            /* Menu prompt:
             * Does user want to format the hard disk? */
            case INSTALL_FORMAT_PARTITION_PROMPT: {
                int answer = inst_formatPartitionDialog(destinationPartition);
                formatPartition = (answer == AD_YESNO_YES);
                goToNext = (answer != AD_CANCELED);
                break;
            }

            /* Menu prompt:
             * Does user want to update MBR and set the partition active? */
            case INSTALL_MBR_ACTIVE_BOOT_PROMPT: {
                int answer = inst_askUserToOverwriteMBRAndSetActive(destinationPartition);
                setActiveAndDoMBR = (answer == AD_YESNO_YES);
                goToNext = (answer != AD_CANCELED);
                break;
            }


            /* Menu prompt:
             * Fast / Slow non-PNP HW detection? */
            case INSTALL_REGISTRY_VARIANT_PROMPT: {
                registryUnpackFile = inst_askUserForRegistryVariant();
                goToNext = (registryUnpackFile != NULL);
                break;
            }

            /* Menu prompt:
             * Does the user want to install the base driver package? */
            case INSTALL_INTEGRATED_DRIVERS_PROMPT: {
                // It's optional, if the file doesn't exist, we don't have to ask
                if (util_fileExists(inst_getCDFilePath(osVariantIndex, INST_DRIVER_FILE))) {
                    int response = inst_showDriverPrompt();
                    installDrivers = (response == AD_YESNO_YES);
                    goToNext = (response != AD_CANCELED);
                } else {
                    installDrivers = false;
                }

                break;
            }


            /* Do the actual install */
            case INSTALL_DO_INSTALL: {
                // Format partition
                if (formatPartition)
                    installSuccess = inst_formatPartition(destinationPartition);

                if (!installSuccess) {
                    inst_showFailedFormat(destinationPartition);
                    currentStep = INSTALL_MAIN_MENU;
                    continue;
                }

                installSuccess = util_mountPartition(destinationPartition);
                // If mounting failed, we will display a message and go back after.

                if (!installSuccess) {
                    inst_showFailedMount(destinationPartition);
                    currentStep = INSTALL_MAIN_MENU;
                    continue;
                }

                // sourceFile is already opened at this point for readahead prebuffering
                installSuccess = inst_copyFiles(sourceFile, destinationPartition->mountPath, "Sistema Operacional");
                mappedFile_close(sourceFile);

                if (!installSuccess) {
                    inst_showFailedCopy(INST_DRIVER_FILE);
                    currentStep = INSTALL_MAIN_MENU;
                    continue;
                }

                // If the main data copy was successful, we move on to the driver file
                if (installSuccess && installDrivers) {
                    sourceFile = inst_openSourceFile(osVariantIndex, INST_DRIVER_FILE, readahead);
                    QI_ASSERT(sourceFile && "Falha ao abrir o arquivo de driver");
                    installSuccess = inst_copyFiles(sourceFile, destinationPartition->mountPath, "Biblioteca de Drivers");
                    mappedFile_close(sourceFile);
                }

                if (!installSuccess) {
                    inst_showFailedCopy(INST_DRIVER_FILE);
                    currentStep = INSTALL_MAIN_MENU;
                    continue;
                }

                // If driver data copy was successful, install registry for selceted hardware detection variant
                if (installSuccess) {
                    sourceFile = inst_openSourceFile(osVariantIndex, registryUnpackFile, readahead);
                    QI_ASSERT(sourceFile && "Falha ao abrir o arquivo de registro");
                    installSuccess = inst_copyFiles(sourceFile, destinationPartition->mountPath, "Registro");
                    mappedFile_close(sourceFile);
                }

                if (!installSuccess) {
                    inst_showFailedCopy(registryUnpackFile);
                    currentStep = INSTALL_MAIN_MENU;
                    continue;
                }

                util_unmountPartition(destinationPartition);

                // Final step: update MBR, boot sector and boot flag.
                if (installSuccess && setActiveAndDoMBR) {
                    installSuccess = inst_setupBootSectorAndMBR(destinationPartition, setActiveAndDoMBR);
                }

                if (installSuccess) {                    
                    doReboot = inst_showSuccessAndAskForReboot();
                } else {
                    inst_showFailMessage();
                }

                quit = true;

                break;
            }           

        }
        currentStep += goToNext ? 1 : -1;
    }

    // Flush filesystem writes clear screen yadayada...

    sync();

    util_hardDiskArrayDestroy(hda);

    system("clear");

    if (doReboot) {
        reboot(RB_AUTOBOOT);
    }

    ad_deinit();

    return installSuccess;
}
