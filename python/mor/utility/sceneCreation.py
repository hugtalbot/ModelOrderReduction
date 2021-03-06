# -*- coding: utf-8 -*-
###############################################################################
#            Model Order Reduction plugin for SOFA                            #
#                         version 1.0                                         #
#                       Copyright © Inria                                     #
#                       All rights reserved                                   #
#                       2018                                                  #
#                                                                             #
# This software is under the GNU General Public License v2 (GPLv2)            #
#            https://www.gnu.org/licenses/licenses.en.html                    #
#                                                                             #
#                                                                             #
#                                                                             #
# Authors: Olivier Goury, Felix Vanneste                                      #
#                                                                             #
# Contact information: https://project.inria.fr/modelorderreduction/contact   #
###############################################################################

import sys

try:
    from splib.animation import animate
    from splib.scenegraph import *
except:
    raise ImportError("ModelOrderReduction plugin depend on SPLIB"\
                     +"Please install it : https://github.com/SofaDefrost/STLIB")

from mor.wrapper import replaceAndSave

forceFieldImplemented = {
                            'HyperReducedTetrahedronFEMForceField':'tetrahedra',
                            'HyperReducedTriangleFEMForceField':'triangles',
                            'HyperReducedRestShapeSpringsForceField':'points'
                        }

import yaml

tmp = 0

def removeObject(obj):
    obj.getContext().removeObject(obj)

def removeObjects(objects):
    for obj in objects :
        removeObject(obj)

def removeNode(node):
    myParent = node.getParents()[0]
    myParent.removeChild(node)

def removeNodes(nodes):
    for node in nodes:
        removeChild(node)

def getNodeSolver(node):
    solver = []
    for obj in node.getObjects():
        className = obj.getClassName()
        if className.find('Solver') != -1 or className == 'EulerImplicit' or className == 'GenericConstraintCorrection':
            # print obj.getName()
            solver.append(obj)
    return solver

def getContainer(node):
    container = None
    for obj in node.getObjects():
        className = obj.getClassName()
        if className.find('Container') != -1: # className.find('Loader') != -1 or
            # print obj.getName()
            container = obj
    return container

def searchObjectClassInGraphScene(node,toFind):
    '''
        Args:
        node (Sofa.node):     Sofa node in wich we are working

        toFind (str):  className we want to find

        Description:

            Search in the Graph scene recursively for all the node
            with the same className as toFind
    '''
    class Namespace(object):
        pass
    tmp = Namespace()
    tmp.results = []

    if not isinstance(toFind,str):
        raise Exception("toFind is either a string or a list of string")

    def search(node,toFind):

        for obj in node.getObjects():
            if obj.getClassName() == toFind:
                tmp.results.append(obj)

        for child in node.getChildren():
            search(child,toFind)

    search(node,toFind)

    return tmp.results

def searchPlugin(rootNode,pluginName):
    found = False
    plugins = searchObjectClassInGraphScene(rootNode,"RequiredPlugin")
    for plugin in plugins:
        for name in plugin.pluginName:
            if name == ["pluginName"]:
                found = True
    return found

def addPlugin(rootNode,pluginName):
    if not searchPlugin(rootNode,pluginName):
        rootNode.createObject('RequiredPlugin', pluginName=pluginName)

def addAnimation(rootNode,phase,timeExe,dt,listObjToAnimate):
    '''
        FOR all node find to animate animate only the one moving -> phase 1/0

        If DEFAULT :
          - SEARCH here for the obj to animate & its valueToIncrement
        Else :
          - GIVE obj name to work with & its valueToIncrement

        give to animate :
          - the obj to work with & its valueToIncrement
          if DEFAULT :
              - the animation function will be defaultShaking
              - the general param lis(range,period,increment)
          else :
              - give the new animation function
              - param lis(...)
    '''
    # Search node to animate

    toAnimate = []
    for obj in listObjToAnimate:
        node = get(rootNode,obj.location)
        print(node.name)
        toAnimate.append(node)

    if len(toAnimate) != len(listObjToAnimate):
        raise Exception("All Obj/Node to animate haven't benn found")

    tmp = 0
    for objToAnimate in listObjToAnimate:
        if phase[tmp] :
            if type(toAnimate[tmp]).__name__ == "Node":
                objToAnimate.item = toAnimate[tmp]
                for obj in objToAnimate.item.getObjects():
                    # print(obj.getClassName())
                    if obj.getClassName() ==  'CableConstraint' or obj.getClassName() ==  'SurfacePressureConstraint':
                        objToAnimate.item = obj
                        objToAnimate.params["dataToWorkOn"] = 'value'

            else :
                objToAnimate.item = toAnimate[tmp]

            if objToAnimate.item and objToAnimate.params["dataToWorkOn"]:
                objToAnimate.duration = timeExe

                animate(objToAnimate.animFct, {'objToAnimate':objToAnimate,'dt':dt}, objToAnimate.duration)
                print("Animate "+objToAnimate.location+" of type "+objToAnimate.item.getClassName()+"\nwith parameters :\n"+str(objToAnimate.params))

            else:
                print("Found Nothing to animate in "+str(objToAnimate.location))

        tmp += 1

def modifyGraphScene(rootNode,nbrOfModes,newParam,save=False):

    modesPositionStr = '0'
    for i in range(1,nbrOfModes):
        modesPositionStr = modesPositionStr + ' 0'
    argMecha = {'template':'Vec1d','position':modesPositionStr}

    for item in newParam :
        pathTmp , param = item
        print('pathTmp -----------------> '+pathTmp)
        try :
            node = get(rootNode,pathTmp[1:])
            solver = getNodeSolver(node)
            print("node.getPathName()",node.getPathName())
            if node.getPathName() == pathTmp:
                if 'paramMappedMatrixMapping' in param:
                    print('Create new child modelMOR and move node in it')

                    myParent = node.getParents()[0]
                    modelMOR = myParent.createChild(node.name+'_MOR')
                    modelMOR.moveChild(node)

                    for obj in solver:
                        # print('To move!')
                        node.removeObject(obj)
                        node.getParents()[0].addObject(obj)

                    modelMOR.createObject('MechanicalObject', **argMecha)

                    # print param['paramMappedMatrixMapping']
                    modelMOR.createObject('MechanicalMatrixMapperMOR', **param['paramMappedMatrixMapping'] )
                    # print 'Create MechanicalMatrixMapperMOR in modelMOR'

                    if save:
                        replaceAndSave.myMORModel.append(('MechanicalObject',argMecha))
                        replaceAndSave.myMORModel.append(('MechanicalMatrixMapperMOR',param['paramMappedMatrixMapping']))

                    if 'paramMORMapping' in param:
                        #Find MechanicalObject name to be able to save to link it to the ModelOrderReductionMapping
                        param['paramMORMapping']['output'] = '@./'+node.getMechanicalState().name
                        if save:
                            replaceAndSave.myModel[pathTmp].append(('ModelOrderReductionMapping',param['paramMORMapping']))

                        node.createObject('ModelOrderReductionMapping', **param['paramMORMapping'])
                        print ("Create ModelOrderReductionMapping in node")
                    # else do error !!
        except :
            print("Problem with path : "+pathTmp[1:])

def saveElements(rootNode,dt,forcefield):
    import numpy as np

    def save(node,container,valueType, **param):
        global tmp
        elements = container.findData(valueType).value
        np.savetxt('reducedFF_'+ node.name + '_' + str(tmp)+'_'+valueType+'_elmts.txt', elements,fmt='%i')
        tmp += 1
        print('save : '+'elmts_'+node.name+' from '+container.name+' with value Type '+valueType)

    # print('--------------------->  ',forcefield)
    for objPath in forcefield:
        nodePath = '/'.join(objPath.split('/')[:-1])
        # print(nodePath,objPath)
        obj = get(rootNode,objPath[1:])
        node = get(rootNode,nodePath[1:])

        if obj.getClassName() == 'HyperReducedRestShapeSpringsForceField':
            container = obj
        else:
            container = getContainer(node)

        if obj.getClassName() in forceFieldImplemented and container:
            valueType = forceFieldImplemented[obj.getClassName()]

            # print('--------------------->  ',valueType)

            if valueType:
                animate(save, {"node" : node ,'container' : container, 'valueType' : valueType, 'startTime' : 0}, 0)

def createDebug(rootNode,paramWrapper,stateFile="stateFile.state"):

    nodeName = []
    nodes = []
    for item in paramWrapper :
        path , param = item
        node = get(rootNode,path[1:])
        solver = getNodeSolver(node)
        removeObjects(solver)

        nodeName.append(node.name)
        nodes.append(node)


    for obj in rootNode.getObjects():
        rootNode.removeObject(obj)

    rootNode.createObject('VisualStyle', displayFlags='showForceFields')
    rootNode.dt = 1

    for child in rootNode.getChildren():
        rootNode.removeChild(child)

    for node in nodes:
        for child in node.getChildren():
            if not (child.name in nodeName):
                # print '--------------------------> remove   '+child.name
                node.removeChild(child)

    nodes[0].createObject('ReadState', filename=stateFile)
    rootNode.addChild(nodes[0])