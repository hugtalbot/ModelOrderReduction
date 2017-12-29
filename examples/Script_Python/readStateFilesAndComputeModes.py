## Usage: python readStateFilesAndComputeModes.py argv[1] argv[2] argv[3] argv[4] argv[5]
##
##               arg1 = stateFilename           ->  Path to state file
##               arg2 = initPositionFilename    ->  Path to state file with init pose
##               arg3 = modesFilename           ->  Name of output file
##               arg4 = tol                     ->  Tolerance (more explanation ??)
##               arg5 = addRigidBodyModesBOOL   ->  Bool indicating if the robot has translation mouvement

import yaml
import math
import numpy as np

#from sys import argv
#script, stateFilename, initPositionFilename, modesFilename, tol, addRigidBodyModes = argv

print "Executing readStateFilesAndComputeModes.py\n"

################################################################################################
## Init variables from data in yaml config file

#Load config file
with open("../config.yml", 'r') as ymlfile:
    cfg = yaml.load(ymlfile)

stateFilename =             cfg['stateFile']['pathTodir']+cfg['stateFile']['nameStateFile']+".state"
initPositionFilename =      cfg['stateFile']['pathTodir']+cfg['stateFile']['nameStateFile']+"_init.state"
modesFilePath =             cfg['modes']['pathTodir']
modesFileName =             cfg['modes']['modesFileName']
tol =                       cfg['modes']['tolerance']
addRigidBodyModes =         cfg['modes']['addRigidBodyModes']

verbose = cfg['other']['verbose']

print "readStateFilesAndComputeModes arguments :"
print "     in modesFilePath         :",modesFilePath
print "         -modesFileName           :",modesFileName
print "     -stateFilename           :",stateFilename
print "     -initPositionFilename    :",initPositionFilename
print "     -tolerance               :",tol
print "     -addRigidBodyModes       :",addRigidBodyModes,"\n"
################################################################################################

x0Found = False

#Find Init Pos
finit = open(initPositionFilename,'r')
if verbose : print "Reading file %r:" % initPositionFilename
for line in finit:
    lineSplit = line.split();
    if (lineSplit[0] == "X0="): 
        lineFloat = map(float,lineSplit[1:])
        restPos = []
        restPos.append(lineFloat)
        restPos = np.transpose(restPos)
        x, y = np.shape(restPos)
        if verbose : print "    X0 FOUND : size restPos = ", x, ' ', y
        x0Found = True
        break
finit.close()
if verbose : print "Done reading file %r:" % initPositionFilename,'\n'

#Compute Modes
if x0Found :
    f = open(stateFilename,'r')
    addRigidBodyModes = int(addRigidBodyModes)
    nbSnap = 0
    nbLine = 0
    snapshot = []
    snapshotV = []

    if verbose : print "Reading file %r:" % stateFilename
    for line in f:
        nbLine = nbLine + 1
        lineSplit = line.split();
        if (lineSplit[0] == "X="):  
            lineFloat = map(float,lineSplit[1:])
            snapshot.append(lineFloat);
        if (lineSplit[0] == "V="):  
            lineFloat = map(float,lineSplit[1:])
            snapshotV.append(lineFloat);

    snapshot = np.transpose(snapshot)
    nbDOFs, nbSnap = np.shape(snapshot)
    snapshotDiff = np.zeros((nbDOFs,nbSnap))
    translationModes = np.zeros((nbDOFs,3))

    if verbose : 
        print "    Read",nbLine,"line and found",nbSnap,"snapshot with",nbDOFs,"of DOF"
        print "Done reading file %r:" % stateFilename,'\n'

    for i in range(0,nbSnap):
        snapshotDiff[:,i] = snapshot[:,i] - restPos[:,0]
      
    for i in range(0,nbDOFs/3):
        translationModes[3*i][0] = 1/math.sqrt(nbDOFs/3)
        translationModes[3*i+1][1] = 1/math.sqrt(nbDOFs/3)
        translationModes[3*i+2][2] = 1/math.sqrt(nbDOFs/3)
    if (addRigidBodyModes == 1):
        for i in range(3):
            for j in range(nbSnap-1):    
                snapshotDiff[:,[j]] = snapshotDiff[:,[j]] - (np.matmul(np.transpose(snapshotDiff[:,[j]]),translationModes[:,[i]]))*translationModes[:,[i]]

    U, s, V = np.linalg.svd(snapshotDiff, full_matrices=False)
    sSquare = [i**2 for i in s]
    sumSVD = np.sum(sSquare)

    i = 0
    if verbose : 
        print "Determining number of Modes with a Tolerance of",tol
        #print "    np.sqrt(np.sum(sSquare[i:])/sumSVD) : "
    while (np.sqrt(np.sum(sSquare[i:])/sumSVD) > tol or i==0):
        i = i+1
        #print "            ",np.sqrt(np.sum(sSquare[i:])/sumSVD)


    nbModes = i
    if (addRigidBodyModes == True):    
        print "Concatenating translation modes !!!!!!!!!!!!"
        modesTot = np.concatenate((translationModes, U[:,0:nbModes]), axis=1)
        np.savetxt(modesFilePath+modesFileName, modesTot, header=str(nbDOFs)+' '+str(nbModes+3), comments='', fmt='%10.5f')
    else:
        np.savetxt(modesFilePath+modesFileName, U[:,0:nbModes], header=str(nbDOFs)+' '+str(nbModes), comments='', fmt='%10.5f')
    
    print "Number of modes to reach tolerance: ", nbModes
    print "===> Success readStateFilesAndComputeModes.py\n"
    #print(s)

    f.close()
else: print "XO NOT FOUND"