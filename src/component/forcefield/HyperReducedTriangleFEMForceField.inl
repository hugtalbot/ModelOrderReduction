/******************************************************************************
*            Model Order Reduction plugin for SOFA                            *
*                         version 1.0                                         *
*                       Copyright © Inria                                     *
*                       All rights reserved                                   *
*                       2018                                                  *
*                                                                             *
* This software is under the GNU General Public License v2 (GPLv2)            *
*            https://www.gnu.org/licenses/licenses.en.html                    *
*                                                                             *
*                                                                             *
*                                                                             *
* Authors: Olivier Goury, Felix Vanneste                                      *
*                                                                             *
* Contact information: https://project.inria.fr/modelorderreduction/contact   *
******************************************************************************/
#ifndef SOFA_COMPONENT_FORCEFIELD_HYPERREDUCEDTRIANGLEFEMFORCEFIELD_INL
#define SOFA_COMPONENT_FORCEFIELD_HYPERREDUCEDTRIANGLEFEMFORCEFIELD_INL

#include "HyperReducedTriangleFEMForceField.h"
#include <sofa/core/visual/VisualParams.h>
#include <sofa/core/ObjectFactory.h>
#include <sofa/core/topology/BaseMeshTopology.h>
#include <sofa/helper/gl/template.h>
#include <fstream> // for reading the file
#include <iostream> //for debugging
#include <vector>
#include <sofa/defaulttype/Vec3Types.h>
#include "../loader/MatrixLoader.h"

//#include "config.h"

// #define DEBUG_TRIANGLEFEM

namespace sofa
{

namespace component
{

namespace forcefield
{

using sofa::component::loader::MatrixLoader;


template <class DataTypes>
HyperReducedTriangleFEMForceField<DataTypes>::
HyperReducedTriangleFEMForceField()
    : _mesh(NULL)
    , _indexedElements(NULL)
    , _initialPoints(initData(&_initialPoints, "initialPoints", "Initial Position"))
    , method(LARGE)
    , f_method(initData(&f_method,std::string("large"),"method","large: large displacements, small: small displacements"))
    , f_poisson(initData(&f_poisson,(Real)0.3,"poissonRatio","Poisson ratio in Hooke's law"))
    , f_young(initData(&f_young,(Real)1000.,"youngModulus","Young modulus in Hooke's law"))
    , f_thickness(initData(&f_thickness,(Real)1.,"thickness","Thickness of the elements"))
//    , f_damping(initData(&f_damping,(Real)0.,"damping","Ratio damping/stiffness"))
    , f_planeStrain(initData(&f_planeStrain,false,"planeStrain","Plane strain or plane stress assumption"))
    , d_prepareECSW(initData(&d_prepareECSW,false,"prepareECSW","Save data necessary for the construction of the reduced model"))
    , d_nbModes(initData(&d_nbModes,unsigned(3),"nbModes","Number of modes when preparing the ECSW method only"))
    , d_nbTrainingSet(initData(&d_nbTrainingSet,unsigned(40),"nbTrainingSet","When preparing the ECSW, size of the training set"))
    , d_periodSaveGIE(initData(&d_periodSaveGIE,unsigned(5),"periodSaveGIE","When prepareECSW is true, the values of Gie are taken every periodSaveGIE timesteps."))
    , d_performECSW(initData(&d_performECSW,false,"performECSW","Use the reduced model with the ECSW method"))
    , d_modesPath(initData(&d_modesPath,std::string("modes.txt"),"modesPath","Path to the file containing the modes (useful only for preparing ECSW)"))
    , d_RIDPath(initData(&d_RIDPath,std::string("reducedIntegrationDomain.txt"),"RIDPath","Path to the Reduced Integration domain when performing the ECSW method"))
    , d_weightsPath(initData(&d_weightsPath,std::string("weights.txt"),"weightsPath","Path to the weights when performing the ECSW method"))
{}

template <class DataTypes>
HyperReducedTriangleFEMForceField<DataTypes>::~HyperReducedTriangleFEMForceField()
{
    f_poisson.setRequired(true);
    f_young.setRequired(true);
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::init()
{
    this->Inherited::init();
    if (f_method.getValue() == "small")
        method = SMALL;
    else if (f_method.getValue() == "large")
        method = LARGE;

    //    serr<<"HyperReducedTriangleFEMForceField<DataTypes>::init(), node = "<<this->getContext()->getName()<<sendl;
    _mesh = this->getContext()->getMeshTopology();

    if( _mesh )
        sout<<"HyperReducedTriangleFEMForceField<DataTypes>::init, mesh has " <<_mesh->getTriangles().size() <<" triangles and " << _mesh->getQuads().size() << " quads" << sendl;

    if (_mesh==NULL || (_mesh->getTriangles().empty() && _mesh->getNbQuads()<=0))
    {
        serr << "ERROR(HyperReducedTriangleFEMForceField): Need a MeshTopology with triangles or quads."<<sendl;
        return;
    }
    if (!_mesh->getTriangles().empty())
    {
        _indexedElements = & (_mesh->getTriangles());
    }
    else
    {
        sofa::core::topology::BaseMeshTopology::SeqTriangles* trias = new sofa::core::topology::BaseMeshTopology::SeqTriangles;
        int nbcubes = _mesh->getNbQuads();
        trias->reserve(nbcubes*2);
        for (int i=0; i<nbcubes; i++)
        {
            sofa::core::topology::BaseMeshTopology::Quad q = _mesh->getQuad(i);
            trias->push_back(Element(q[0],q[1],q[2]));
            trias->push_back(Element(q[0],q[2],q[3]));
        }
        _indexedElements = trias;
    }

    if (_initialPoints.getValue().size() == 0)
    {
        const VecCoord& p = this->mstate->read(core::ConstVecCoordId::restPosition())->getValue();
        _initialPoints.setValue(p);
    }

    _strainDisplacements.resize(_indexedElements->size());
    _rotations.resize(_indexedElements->size());


    initSmall();
    initLarge();
    computeMaterialStiffnesses();

    if (d_prepareECSW.getValue()){
        MatrixLoader<Eigen::MatrixXd>* matLoader = new MatrixLoader<Eigen::MatrixXd>();
        matLoader->setFileName(d_modesPath.getValue());
        matLoader->load();
        matLoader->getMatrix(m_modes);
        delete matLoader;
        m_modes.conservativeResize(Eigen::NoChange,d_nbModes.getValue());


        Gie.resize(d_nbTrainingSet.getValue()*d_nbModes.getValue());
        b_ECSW.resize(d_nbTrainingSet.getValue()*d_nbModes.getValue());
        for (unsigned int i = 0; i < d_nbTrainingSet.getValue()*d_nbModes.getValue(); i++)
        {
            b_ECSW[i] = 0;
            Gie[i].resize(_indexedElements->size());
            for (unsigned int j = 0; j < _indexedElements->size(); j++)
            {
                Gie[i][j] = 0;
            }
        }
    }
    if (d_performECSW.getValue())
    {
        MatrixLoader<Eigen::VectorXd>* weightsMatLoader = new MatrixLoader<Eigen::VectorXd>();
        weightsMatLoader->setFileName(d_weightsPath.getValue());
        weightsMatLoader->load();
        weightsMatLoader->getMatrix(weights);
        delete weightsMatLoader;

        MatrixLoader<Eigen::VectorXi>* RIDMatLoader = new MatrixLoader<Eigen::VectorXi>();
        RIDMatLoader->setFileName(d_RIDPath.getValue());
        RIDMatLoader->load();
        RIDMatLoader->getMatrix(reducedIntegrationDomain);
        delete RIDMatLoader;
        m_RIDsize = reducedIntegrationDomain.rows();
    }
    else
    {
        m_RIDsize = _indexedElements->size();  // the reduced integration contains all the elements in this case.
        reducedIntegrationDomain.resize(m_RIDsize);
        for (unsigned int i = 0; i<m_RIDsize; i++)
            reducedIntegrationDomain(i) = i;
    }

}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::reinit()
{
    if (f_method.getValue() == "small")
        method = SMALL;
    else if (f_method.getValue() == "large")
        method = LARGE;


    //    initSmall();  // useful ? The rotations are recomputed later
    initLarge();  // compute the per-element strain-displacement matrices
    computeMaterialStiffnesses();
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::addForce(const core::MechanicalParams* /* mparams */, DataVecDeriv& f, const DataVecCoord& x, const DataVecDeriv& /* v */)
{
    VecDeriv& f1 = *f.beginEdit();
    const VecCoord& x1 = x.getValue();

    f1.resize(x1.size());

    if(method==SMALL)
    {
        typename VecElement::const_iterator it;
        unsigned int i(0);

        for(it = _indexedElements->begin() ; it != _indexedElements->end() ; ++it, ++i)
        {
            accumulateForceSmall( f1, x1, i, true );
        }
    }
    else
    {
        typename VecElement::const_iterator it;
        unsigned int i(0);
        if (d_performECSW.getValue()){
            for( i = 0 ; i<m_RIDsize ;++i)
            {
                accumulateForceLarge( f1, x1, reducedIntegrationDomain(i), true );
            }
        }
        else
        {
            for(it = _indexedElements->begin() ; it != _indexedElements->end() ; ++it, ++i)
            {
                accumulateForceLarge( f1, x1, i, true );
            }
        }
        if (d_prepareECSW.getValue())
        {
            int numTest = this->getContext()->getTime()/this->getContext()->getDt();
            if (numTest%d_periodSaveGIE.getValue() == 0)       // A new value was taken
            {
                numTest = numTest/d_periodSaveGIE.getValue();
                if (numTest < d_nbTrainingSet.getValue()){
                    std::stringstream gieFileNameSS;
                    gieFileNameSS << this->name << "_Gie.txt";
                    std::string gieFileName = gieFileNameSS.str();
                    std::ofstream myfileGie (gieFileName, std::fstream::app);
                    msg_info(this) << "Storing case number " << numTest+1 << " in " << gieFileName << " ...";
                    for (unsigned int k=numTest*d_nbModes.getValue(); k<(numTest+1)*d_nbModes.getValue();k++){
                        for (unsigned int l=0;l<_indexedElements->size();l++){
                            myfileGie << Gie[k][l] << " ";
                        }
                        myfileGie << std::endl;
                    }
                    myfileGie.close();
                    msg_info(this) << "-------------  Storing Done -------------";
                }
                else
                {
                    msg_info(this) << d_nbTrainingSet.getValue() << "were already stored. Learning phase completed.";
                }
            }
        }
    }
    f.endEdit();
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::addDForce(const core::MechanicalParams* mparams, DataVecDeriv& df, const DataVecDeriv& dx)
{
    VecDeriv& df1 = *df.beginEdit();
    const VecDeriv& dx1 = dx.getValue();
    Real kFactor = (Real)mparams->kFactorIncludingRayleighDamping(this->rayleighStiffness.getValue());

    Real h=1;
    df1.resize(dx1.size());

    if (method == SMALL)
    {
        applyStiffnessSmall( df1, h, dx1, kFactor );
    }
    else
    {
        applyStiffnessLarge( df1, h, dx1, kFactor );
    }

    df.endEdit();
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::applyStiffness( VecCoord& v, Real h, const VecCoord& x, const SReal &kFactor )
{
    if (method == SMALL)
    {
        applyStiffnessSmall( v, h, x, kFactor );
    }
    else
    {
        applyStiffnessLarge( v, h, x, kFactor );
    }
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::computeStrainDisplacement( StrainDisplacement &J, Coord /*a*/, Coord b, Coord c )
{
#ifdef DEBUG_TRIANGLEFEM
    sout << "HyperReducedTriangleFEMForceField::computeStrainDisplacement"<<sendl;
#endif

    //    //Coord ab_cross_ac = cross(b, c);
    Real determinant = b[0] * c[1]; // Surface * 2

    J[0][0] = J[1][2] = -c[1] / determinant;
    J[0][2] = J[1][1] = (c[0] - b[0]) / determinant;
    J[2][0] = J[3][2] = c[1] / determinant;
    J[2][2] = J[3][1] = -c[0] / determinant;
    J[4][0] = J[5][2] = 0;
    J[4][2] = J[5][1] = b[0] / determinant;
    J[1][0] = J[3][0] = J[5][0] = J[0][1] = J[2][1] = J[4][1] = 0;

    /* The following formulation is actually equivalent:
      Let
      | alpha1 alpha2 alpha3 |                      | 1 xa ya |
      | beta1  beta2  beta3  | = be the inverse of  | 1 xb yb |
      | gamma1 gamma2 gamma3 |                      | 1 xc yc |

      The strain-displacement matrix is:
      | beta1  0       beta2  0        beta3  0      |
      | 0      gamma1  0      gamma2   0      gamma3 | / (2*A)
      | gamma1 beta1   gamma2 beta2    gamma3 beta3  |

      where A is the area of the triangle and 2*A is the determinant of the matrix with the xa,ya,xb...


      Since a0=a1=b1=0, the matrix is triangular and its inverse is:
      |  1              0              0  |
      | -1/xb           1/xb           0  |
      | -(1-xc/xb)/yc  -xc/(xb*yc)   1/yc |

      our strain-displacement matrix is:
      | -1/xb           0             1/xb         0            0     0    |
      | 0              -(1-xc/xb)/yc  0            -xc/(xb*yc)  0     1/yc |
      | -(1-xc/xb)/yc  -1/xb          -xc/(xb*yc)  1/xb         1/yc  0    |
      */

    //    Real beta1  = -1/b[0];
    //    Real beta2  =  1/b[0];
    //    Real gamma1 = (c[0]/b[0]-1)/c[1];
    //    Real gamma2 = -c[0]/(b[0]*c[1]);
    //    Real gamma3 = 1/c[1];

    //    // The transpose of the strain-displacement matrix is thus:
    //    J[0][0] = J[1][2] = beta1;
    //    J[0][1] = J[1][0] = 0;
    //    J[0][2] = J[1][1] = gamma1;

    //    J[2][0] = J[3][2] = beta2;
    //    J[2][1] = J[3][0] = 0;
    //    J[2][2] = J[3][1] = gamma2;

    //    J[4][0] = J[5][2] = 0;
    //    J[4][1] = J[5][0] = 0;
    //    J[4][2] = J[5][1] = gamma3;



}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::computeMaterialStiffnesses()
{
    _materialsStiffnesses.resize(_indexedElements->size());
    const VecCoord& p= _initialPoints.getValue();


    for(unsigned i = 0; i < _indexedElements->size(); ++i)
    {
        Index a = (*_indexedElements)[i][0];
        Index b = (*_indexedElements)[i][1];
        Index c = (*_indexedElements)[i][2];
        Real triangleVolume = (Real)0.5 * f_thickness.getValue() * cross( p[b]-p[a], p[c]-p[a] ).norm();

        if( f_planeStrain.getValue() == true )
        {
            _materialsStiffnesses[i][0][0] = 1-f_poisson.getValue();
            _materialsStiffnesses[i][0][1] = f_poisson.getValue();
            _materialsStiffnesses[i][0][2] = 0;
            _materialsStiffnesses[i][1][0] = f_poisson.getValue();
            _materialsStiffnesses[i][1][1] = 1-f_poisson.getValue();
            _materialsStiffnesses[i][1][2] = 0;
            _materialsStiffnesses[i][2][0] = 0;
            _materialsStiffnesses[i][2][1] = 0;
            _materialsStiffnesses[i][2][2] = 0.5f - f_poisson.getValue();

            _materialsStiffnesses[i] *= f_young.getValue() / ( (1 + f_poisson.getValue()) * (1-2*f_poisson.getValue()) ) * triangleVolume;
        }
        else // plane stress
        {
            _materialsStiffnesses[i][0][0] = 1;
            _materialsStiffnesses[i][0][1] = f_poisson.getValue();
            _materialsStiffnesses[i][0][2] = 0;
            _materialsStiffnesses[i][1][0] = f_poisson.getValue();
            _materialsStiffnesses[i][1][1] = 1;
            _materialsStiffnesses[i][1][2] = 0;
            _materialsStiffnesses[i][2][0] = 0;
            _materialsStiffnesses[i][2][1] = 0;
            _materialsStiffnesses[i][2][2] = 0.5f * (1 - f_poisson.getValue());

            _materialsStiffnesses[i] *= f_young.getValue() / ( (1 - f_poisson.getValue() * f_poisson.getValue())) * triangleVolume;
        }
    }
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::computeForce( Displacement &F, const Displacement &Depl, const MaterialStiffness &K, const StrainDisplacement &J )
{
    defaulttype::Mat<3,6,Real> Jt;
    Jt.transpose( J );

    defaulttype::Vec<3,Real> JtD;

    // Optimisations: The following values are 0 (per computeStrainDisplacement )

    // Jt[0][1]
    // Jt[0][3]
    // Jt[0][4]
    // Jt[0][5]
    // Jt[1][0]
    // Jt[1][2]
    // Jt[1][4]
    // Jt[2][5]


    //	JtD = Jt * Depl;

    JtD[0] = Jt[0][0] * Depl[0] + /* Jt[0][1] * Depl[1] + */ Jt[0][2] * Depl[2]
            /* + Jt[0][3] * Depl[3] + Jt[0][4] * Depl[4] + Jt[0][5] * Depl[5] */ ;

    JtD[1] = /* Jt[1][0] * Depl[0] + */ Jt[1][1] * Depl[1] + /* Jt[1][2] * Depl[2] + */
            Jt[1][3] * Depl[3] + /* Jt[1][4] * Depl[4] + */ Jt[1][5] * Depl[5];

    JtD[2] = Jt[2][0] * Depl[0] + Jt[2][1] * Depl[1] + Jt[2][2] * Depl[2] +
            Jt[2][3] * Depl[3] + Jt[2][4] * Depl[4] /* + Jt[2][5] * Depl[5] */ ;

    defaulttype::Vec<3,Real> KJtD;

    //	KJtD = K * JtD;

    // Optimisations: The following values are 0 (per computeMaterialStiffnesses )

    // K[0][2]
    // K[1][2]
    // K[2][0]
    // K[2][1]

    KJtD[0] = K[0][0] * JtD[0] + K[0][1] * JtD[1] /* + K[0][2] * JtD[2] */;

    KJtD[1] = K[1][0] * JtD[0] + K[1][1] * JtD[1] /* + K[1][2] * JtD[2] */;

    KJtD[2] = /* K[2][0] * JtD[0] + K[2][1] * JtD[1] */ + K[2][2] * JtD[2];

    //	F = J * KJtD;


    // Optimisations: The following values are 0 (per computeStrainDisplacement )

    // J[0][1]
    // J[1][0]
    // J[2][1]
    // J[3][0]
    // J[4][0]
    // J[4][1]
    // J[5][0]
    // J[5][2]

    F[0] = J[0][0] * KJtD[0] + /* J[0][1] * KJtD[1] + */ J[0][2] * KJtD[2];

    F[1] = /* J[1][0] * KJtD[0] + */ J[1][1] * KJtD[1] + J[1][2] * KJtD[2];

    F[2] = J[2][0] * KJtD[0] + /* J[2][1] * KJtD[1] + */ J[2][2] * KJtD[2];

    F[3] = /* J[3][0] * KJtD[0] + */ J[3][1] * KJtD[1] + J[3][2] * KJtD[2];

    F[4] = /* J[4][0] * KJtD[0] + J[4][1] * KJtD[1] + */ J[4][2] * KJtD[2];

    F[5] = /* J[5][0] * KJtD[0] + */ J[5][1] * KJtD[1] /* + J[5][2] * KJtD[2] */ ;
}

/*
** SMALL DEFORMATION METHODS
*/
template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::initSmall()
{

#ifdef DEBUG_TRIANGLEFEM
    sout << "HyperReducedTriangleFEMForceField::initSmall"<<sendl;
#endif

    Transformation identity;
    identity[0][0]=identity[1][1]=identity[2][2]=1;
    identity[0][1]=identity[0][2]=0;
    identity[1][0]=identity[1][2]=0;
    identity[2][0]=identity[2][1]=0;
    for(unsigned i=0; i< _indexedElements->size() ; ++i)
    {
        _rotations[i] = identity;
    }
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::accumulateForceSmall( VecCoord &f, const VecCoord &p, Index elementIndex, bool implicit )
{

#ifdef DEBUG_TRIANGLEFEM
    sout << "HyperReducedTriangleFEMForceField::accumulateForceSmall"<<sendl;
#endif

    Index a = (*_indexedElements)[elementIndex][0];
    Index b = (*_indexedElements)[elementIndex][1];
    Index c = (*_indexedElements)[elementIndex][2];

    Coord deforme_a, deforme_b, deforme_c;
    deforme_b = p[b]-p[a];
    deforme_c = p[c]-p[a];
    deforme_a = Coord(0,0,0);

    // displacements
    Displacement D;
    D[0] = 0;
    D[1] = 0;
    D[2] = (_initialPoints.getValue()[b][0]-_initialPoints.getValue()[a][0]) - deforme_b[0];
    D[3] = 0;
    D[4] = (_initialPoints.getValue()[c][0]-_initialPoints.getValue()[a][0]) - deforme_c[0];
    D[5] = (_initialPoints.getValue()[c][1]-_initialPoints.getValue()[a][1]) - deforme_c[1];


    StrainDisplacement J;
    computeStrainDisplacement(J,deforme_a,deforme_b,deforme_c);
    if (implicit)
        _strainDisplacements[elementIndex] = J;

    // compute force on element
    Displacement F;
    computeForce( F, D, _materialsStiffnesses[elementIndex], J );

    f[a] += Coord( F[0], F[1], 0);
    f[b] += Coord( F[2], F[3], 0);
    f[c] += Coord( F[4], F[5], 0);
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::applyStiffnessSmall(VecCoord &v, Real h, const VecCoord &x, const SReal &kFactor)
{

#ifdef DEBUG_TRIANGLEFEM
    sout << "HyperReducedTriangleFEMForceField::applyStiffnessSmall"<<sendl;
#endif

    typename VecElement::const_iterator it;
    unsigned int i(0);

    for(it = _indexedElements->begin() ; it != _indexedElements->end() ; ++it, ++i)
    {
        Index a = (*it)[0];
        Index b = (*it)[1];
        Index c = (*it)[2];

        Displacement X;

        X[0] = x[a][0];
        X[1] = x[a][1];

        X[2] = x[b][0];
        X[3] = x[b][1];

        X[4] = x[c][0];
        X[5] = x[c][1];

        Displacement F;
        computeForce( F, X, _materialsStiffnesses[i], _strainDisplacements[i] );

        v[a] += Coord(-h*F[0], -h*F[1], 0)*kFactor;
        v[b] += Coord(-h*F[2], -h*F[3], 0)*kFactor;
        v[c] += Coord(-h*F[4], -h*F[5], 0)*kFactor;
    }
}

/*
** LARGE DEFORMATION METHODS
*/
template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::initLarge()
{

#ifdef DEBUG_TRIANGLEFEM
    sout << "HyperReducedTriangleFEMForceField::initLarge"<<sendl;
#endif

    _rotatedInitialElements.resize(_indexedElements->size());

    typename VecElement::const_iterator it;
    unsigned int i(0);

    for(it = _indexedElements->begin() ; it != _indexedElements->end() ; ++it, ++i)
    {
        Index a = (*it)[0];
        Index b = (*it)[1];
        Index c = (*it)[2];

        // Rotation matrix (transpose of initial triangle/world)
        // first vector on first edge
        // second vector in the plane of the two first edges
        // third vector orthogonal to first and second
        Transformation R_0_1;
        computeRotationLarge( R_0_1, _initialPoints.getValue(), a, b, c );
        _rotations[i].transpose(R_0_1);

        // coordinates of the triangle vertices in their local frames
        _rotatedInitialElements[i][0] = R_0_1 * _initialPoints.getValue()[a];
        _rotatedInitialElements[i][1] = R_0_1 * _initialPoints.getValue()[b];
        _rotatedInitialElements[i][2] = R_0_1 * _initialPoints.getValue()[c];
        // set the origin of the local frame at vertex a
        _rotatedInitialElements[i][1] -= _rotatedInitialElements[i][0];
        _rotatedInitialElements[i][2] -= _rotatedInitialElements[i][0];
        _rotatedInitialElements[i][0] = Coord(0,0,0);

        computeStrainDisplacement(_strainDisplacements[i], _initialPoints.getValue()[a], _initialPoints.getValue()[b], _initialPoints.getValue()[c] );
    }
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::computeRotationLarge( Transformation &r, const VecCoord &p, const Index &a, const Index &b, const Index &c)
{

#ifdef DEBUG_TRIANGLEFEM
    sout << "HyperReducedTriangleFEMForceField::computeRotationLarge"<<sendl;
#endif

    // first vector on first edge
    // second vector in the plane of the two first edges
    // third vector orthogonal to first and second

    Coord edgex = p[b] - p[a];
    edgex.normalize();

    Coord edgey = p[c] - p[a];
    edgey.normalize();

    Coord edgez;
    edgez = cross(edgex, edgey);
    edgez.normalize();

    edgey = cross(edgez, edgex);
    edgey.normalize();

    r[0][0] = edgex[0];
    r[0][1] = edgex[1];
    r[0][2] = edgex[2];
    r[1][0] = edgey[0];
    r[1][1] = edgey[1];
    r[1][2] = edgey[2];
    r[2][0] = edgez[0];
    r[2][1] = edgez[1];
    r[2][2] = edgez[2];
}

template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::accumulateForceLarge(VecCoord &f, const VecCoord &p, Index elementIndex, bool implicit )
{

#ifdef DEBUG_TRIANGLEFEM
    sout << "HyperReducedTriangleFEMForceField::accumulateForceLarge"<<sendl;
#endif

    std::vector<double> GieUnit;
    if (d_prepareECSW.getValue())
    {
        GieUnit.resize(d_nbModes.getValue());
    }

    // triangle vertex indices
    Index a = (*_indexedElements)[elementIndex][0];
    Index b = (*_indexedElements)[elementIndex][1];
    Index c = (*_indexedElements)[elementIndex][2];

    // Rotation matrix (deformed and displaced Triangle/world)
    Transformation R_2_0, R_0_2;
    computeRotationLarge( R_0_2, p, a, b, c);


    // positions of the deformed points in the local frame
    Coord deforme_a, deforme_b, deforme_c;
    deforme_b = R_0_2 * (p[b]-p[a]);
    deforme_c = R_0_2 * (p[c]-p[a]);

    // displacements in the local frame
    Displacement D;
    D[0] = 0;
    D[1] = 0;
    D[2] = _rotatedInitialElements[elementIndex][1][0] - deforme_b[0];
    D[3] = 0;
    D[4] = _rotatedInitialElements[elementIndex][2][0] - deforme_c[0];
    D[5] = _rotatedInitialElements[elementIndex][2][1] - deforme_c[1];

    // Strain-displacement matrix
    StrainDisplacement B;
    computeStrainDisplacement(B,deforme_a,deforme_b,deforme_c);

    // compute force on element, in local frame
    Displacement F;
    computeForce( F, D, _materialsStiffnesses[elementIndex], B ); // F = Bt.S.B.D

    // project forces to world frame
    R_2_0.transpose(R_0_2);
    if (d_performECSW.getValue()){
        f[a] += weights(elementIndex) * R_2_0 * Coord(F[0], F[1], 0);
        f[b] += weights(elementIndex) * R_2_0 * Coord(F[2], F[3], 0);
        f[c] += weights(elementIndex) * R_2_0 * Coord(F[4], F[5], 0);
    }
    else
    {
        f[a] += R_2_0 * Coord(F[0], F[1], 0);
        f[b] += R_2_0 * Coord(F[2], F[3], 0);
        f[c] += R_2_0 * Coord(F[4], F[5], 0);
    }


    if (d_prepareECSW.getValue())
    {
        int numTest = this->getContext()->getTime()/this->getContext()->getDt();
        if (numTest%d_periodSaveGIE.getValue() == 0)       // Take a measure every periodSaveGIE timesteps
        {

            numTest = numTest/d_periodSaveGIE.getValue();

            for (unsigned int modNum = 0 ; modNum < d_nbModes.getValue() ; modNum++)
            {
                GieUnit[modNum] = 0;
                GieUnit[modNum] += (R_2_0 * Coord(F[0], F[1], 0)) * Coord(m_modes(3*a,modNum),m_modes(3*a+1,modNum),m_modes(3*a+2,modNum));
                GieUnit[modNum] += (R_2_0 * Coord(F[2], F[3], 0)) * Coord(m_modes(3*b,modNum),m_modes(3*b+1,modNum),m_modes(3*b+2,modNum));
                GieUnit[modNum] += (R_2_0 * Coord(F[4], F[5], 0)) * Coord(m_modes(3*c,modNum),m_modes(3*c+1,modNum),m_modes(3*c+2,modNum));

            }
            for (unsigned int i = 0 ; i < d_nbModes.getValue() ; i++)
            {
                if (d_nbModes.getValue()*numTest < d_nbTrainingSet.getValue()*d_nbModes.getValue())
                {
                    Gie[d_nbModes.getValue()*numTest+i][elementIndex] = GieUnit[i];
                    b_ECSW[d_nbModes.getValue()*numTest+i] += GieUnit[i];
                }
            }
        }
    }
    // store for re-use in matrix-vector products
    if(implicit)
    {
        _strainDisplacements[elementIndex] = B;
        _rotations[elementIndex] = R_2_0 ;
    }

}


//template <class DataTypes>
//void HyperReducedTriangleFEMForceField<DataTypes>::accumulateDampingLarge(VecCoord &, Index )
//{

//#ifdef DEBUG_TRIANGLEFEM
//    sout << "HyperReducedTriangleFEMForceField::accumulateDampingLarge"<<sendl;
//#endif

//}


template <class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::applyStiffnessLarge(VecCoord &v, Real h, const VecCoord &x, const SReal &kFactor)
{

#ifdef DEBUG_TRIANGLEFEM
    sout << "HyperReducedTriangleFEMForceField::applyStiffnessLarge"<<sendl;
#endif

    typename VecElement::const_iterator it, it0;
    unsigned int i(0);
    if (d_performECSW.getValue())
    {
        int iECSW;
        it0=_indexedElements->begin();
        for( i = 0 ; i<m_RIDsize ;++i)
        {
            iECSW = reducedIntegrationDomain(i);
            it = it0 + iECSW;
            Index a = (*it)[0];
            Index b = (*it)[1];
            Index c = (*it)[2];

            Transformation R_0_2;
            R_0_2.transpose(_rotations[iECSW]);

            Displacement X;
            Coord x_2;

            x_2 = R_0_2 * x[a];
            X[0] = x_2[0];
            X[1] = x_2[1];

            x_2 = R_0_2 * x[b];
            X[2] = x_2[0];
            X[3] = x_2[1];

            x_2 = R_0_2 * x[c];
            X[4] = x_2[0];
            X[5] = x_2[1];

            Displacement F;
            computeForce( F, X, _materialsStiffnesses[iECSW], _strainDisplacements[iECSW] );
            v[a] += weights(iECSW)*(_rotations[iECSW] * Coord(-h*F[0], -h*F[1], 0))*kFactor;
            v[b] += weights(iECSW)*(_rotations[iECSW] * Coord(-h*F[2], -h*F[3], 0))*kFactor;
            v[c] += weights(iECSW)*(_rotations[iECSW] * Coord(-h*F[4], -h*F[5], 0))*kFactor;
        }
    }
    else
    {
        for(it = _indexedElements->begin() ; it != _indexedElements->end() ; ++it, ++i)
        {
            Index a = (*it)[0];
            Index b = (*it)[1];
            Index c = (*it)[2];

            Transformation R_0_2;
            R_0_2.transpose(_rotations[i]);

            Displacement X;
            Coord x_2;

            x_2 = R_0_2 * x[a];
            X[0] = x_2[0];
            X[1] = x_2[1];

            x_2 = R_0_2 * x[b];
            X[2] = x_2[0];
            X[3] = x_2[1];

            x_2 = R_0_2 * x[c];
            X[4] = x_2[0];
            X[5] = x_2[1];

            Displacement F;
            computeForce( F, X, _materialsStiffnesses[i], _strainDisplacements[i] );
            v[a] += (_rotations[i] * Coord(-h*F[0], -h*F[1], 0))*kFactor;
            v[b] += (_rotations[i] * Coord(-h*F[2], -h*F[3], 0))*kFactor;
            v[c] += (_rotations[i] * Coord(-h*F[4], -h*F[5], 0))*kFactor;
        }
    }
}

template<class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::draw(const core::visual::VisualParams* vparams)
{
#ifndef SOFA_NO_OPENGL
    if (!vparams->displayFlags().getShowForceFields())
        return;
    //     if (!this->_object)
    //         return;

    if (vparams->displayFlags().getShowWireFrame())
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    const VecCoord& x = this->mstate->read(core::ConstVecCoordId::position())->getValue();

    glDisable(GL_LIGHTING);

    glBegin(GL_TRIANGLES);
    typename VecElement::const_iterator it, it0;
    it0 = _indexedElements->begin();
    for(unsigned i = 0 ; i<m_RIDsize ;++i)
    {
        it = it0 + reducedIntegrationDomain(i);
        Index a = (*it)[0];
        Index b = (*it)[1];
        Index c = (*it)[2];

        glColor4f(0,1,0,1);
        helper::gl::glVertexT(x[a]);
        glColor4f(0,0.5,0.5,1);
        helper::gl::glVertexT(x[b]);
        glColor4f(0,0,1,1);
        helper::gl::glVertexT(x[c]);
    }
    glEnd();

    if (vparams->displayFlags().getShowWireFrame())
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif /* SOFA_NO_OPENGL */
}

template<class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::computeElementStiffnessMatrix( StiffnessMatrix& S, StiffnessMatrix& SR, const MaterialStiffness &K, const StrainDisplacement &J, const Transformation& Rot )
{
    defaulttype::MatNoInit<3, 6, Real> Jt;
    Jt.transpose( J );

    defaulttype::MatNoInit<6, 6, Real> JKJt;
    JKJt = J*K*Jt;  // in-plane stiffness matrix, 6x6

    // stiffness JKJt expanded to 3 dimensions
    defaulttype::Mat<9, 9, Real> Ke; // initialized to 0
    // for each 2x2 block i,j
    for(unsigned i=0; i<3; i++)
    {
        for(unsigned j=0; j<3; j++)
        {
            // copy the block in the expanded matrix
            for(unsigned k=0; k<2; k++)
                for(unsigned l=0; l<2; l++)
                    Ke[3*i+k][3*j+l] = JKJt[2*i+k][2*j+l];
        }
    }

    // rotation matrices. TODO: use block-diagonal matrices, more efficient.
    defaulttype::Mat<9, 9, Real> RR,RRt; // initialized to 0
    for(int i=0; i<3; ++i)
        for(int j=0; j<3; ++j)
        {
            RR[i][j]=RR[i+3][j+3]=RR[i+6][j+6]=Rot[i][j];
            RRt[i][j]=RRt[i+3][j+3]=RRt[i+6][j+6]=Rot[j][i];
        }

    S = RR*Ke;
    SR = S*RRt;
}

template<class DataTypes>
void HyperReducedTriangleFEMForceField<DataTypes>::addKToMatrix(sofa::defaulttype::BaseMatrix *mat, SReal k, unsigned int &offset)
{
    if (d_performECSW.getValue())
    {
        int iECSW;
        for(unsigned i = 0 ; i<m_RIDsize ;++i)
        {
            iECSW = reducedIntegrationDomain(i);
            StiffnessMatrix JKJt,RJKJtRt;
            computeElementStiffnessMatrix(JKJt, RJKJtRt, _materialsStiffnesses[iECSW], _strainDisplacements[iECSW], _rotations[iECSW]);
            this->addToMatrix(mat,offset,(*_indexedElements)[iECSW],weights(iECSW)*RJKJtRt,-k);

        }
    }
    else
    {
        for(unsigned i=0; i< _indexedElements->size() ; i++)
        {
            StiffnessMatrix JKJt,RJKJtRt;
            computeElementStiffnessMatrix(JKJt, RJKJtRt, _materialsStiffnesses[i], _strainDisplacements[i], _rotations[i]);
            this->addToMatrix(mat,offset,(*_indexedElements)[i],RJKJtRt,-k);
        }
    }
}


} // namespace forcefield

} // namespace component

} // namespace sofa

#endif // #ifndef SOFA_COMPONENT_FORCEFIELD_HYPERREDUCEDTRIANGLEFEMFORCEFIELD_INL
